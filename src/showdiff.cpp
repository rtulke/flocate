#include "db.h"
#include "metadata.h"
#include "metadata_codec.h"
#include "metadata_hash.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <numeric>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zstd.h>

using namespace std;

namespace {

// Pruning settings updatedb stored in the database's configuration block.
// Live mode honours them so that the comparison covers the same tree the
// database does.
struct PruneConfig {
	bool prune_bind_mounts = false;
	vector<string> prunefs;  // Upper-cased filesystem type names.
	vector<string> prunenames;
	vector<string> prunepaths;
};

struct Snapshot {
	vector<string> paths;  // Sorted; metadata[i] belongs to paths[i].
	vector<FileMetadata> metadata;
	MetadataHashKind hash_kind = MetadataHashKind::None;
	PruneConfig prune;
	string scan_root;  // Directory updatedb was run with (-U), derived from the paths.
};

struct EventFilter {
	bool show_added = true;
	bool show_removed = true;
	bool show_modified = true;
	bool filter_specified = false;

	void enable(HistoryEventKind kind)
	{
		if (!filter_specified) {
			show_added = show_removed = show_modified = false;
			filter_specified = true;
		}
		switch (kind) {
		case HistoryEventKind::Added:
			show_added = true;
			break;
		case HistoryEventKind::Removed:
			show_removed = true;
			break;
		case HistoryEventKind::Modified:
			show_modified = true;
			break;
		case HistoryEventKind::RunMarker:
			break;
		}
	}

	bool accepts(HistoryEventKind kind) const
	{
		switch (kind) {
		case HistoryEventKind::Added:
			return show_added;
		case HistoryEventKind::Removed:
			return show_removed;
		case HistoryEventKind::Modified:
			return show_modified;
		case HistoryEventKind::RunMarker:
			return true;
		}
		return true;
	}
};

// Whether atime differences alone count as a modification in diff and live mode.
bool compare_atime = false;
// Emit one JSON object per line instead of the human-readable format.
bool json_output = false;

// ---------------------------------------------------------------------------
// Database reading

bool pread_fully(int fd, void *buf, size_t len, off_t offset)
{
	char *ptr = static_cast<char *>(buf);
	while (len > 0) {
		ssize_t ret = pread(fd, ptr, len, offset);
		if (ret == 0) {
			return false;
		}
		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		ptr += ret;
		len -= ret;
		offset += ret;
	}
	return true;
}

bool section_fits(uint64_t offset, uint64_t length, uint64_t file_size)
{
	return length == 0 || (offset <= file_size && length <= file_size - offset);
}

// Reads and sanity-checks the header. Every section the caller may touch is
// verified to lie inside the file, so a corrupt or foreign file cannot make us
// allocate or read arbitrary amounts.
bool read_header(int fd, const char *filename, Header *hdr)
{
	struct stat sb;
	if (fstat(fd, &sb) != 0) {
		perror(filename);
		return false;
	}
	const uint64_t file_size = sb.st_size;
	if (file_size < sizeof(*hdr) || !pread_fully(fd, hdr, sizeof(*hdr), 0) ||
	    memcmp(hdr->magic, "\0flocate", 8) != 0) {
		fprintf(stderr, "%s: not a flocate database\n", filename);
		return false;
	}
	if (hdr->version != 1 && hdr->version != 2) {
		fprintf(stderr, "%s: unsupported database version %u\n", filename, hdr->version);
		return false;
	}
	if (hdr->max_version < 3) {
		fprintf(stderr, "%s: database was built without metadata support (max_version=%u); rebuild it with updatedb\n",
		        filename, hdr->max_version);
		return false;
	}
	const uint64_t index_bytes = (static_cast<uint64_t>(hdr->num_docids) + 1) * sizeof(uint64_t);
	if (!section_fits(hdr->filename_index_offset_bytes, index_bytes, file_size) ||
	    !section_fits(hdr->zstd_dictionary_offset_bytes, hdr->zstd_dictionary_length_bytes, file_size) ||
	    !section_fits(hdr->conf_block_offset_bytes, hdr->conf_block_length_bytes, file_size) ||
	    !section_fits(hdr->metadata_offset_bytes, hdr->metadata_length_bytes, file_size) ||
	    !section_fits(hdr->history_offset_bytes, hdr->history_length_bytes, file_size)) {
		fprintf(stderr, "%s: corrupt database (section outside of file)\n", filename);
		return false;
	}
	if ((hdr->metadata_length_bytes != 0 || hdr->history_length_bytes != 0) &&
	    !(hdr->metadata_flags & METADATA_FLAG_POSIX_V2)) {
		fprintf(stderr, "%s: metadata format (flags=%u) is not supported by this version; rebuild the database with updatedb\n",
		        filename, hdr->metadata_flags);
		return false;
	}
	return true;
}

bool read_blob(int fd, const char *filename, uint64_t offset, uint64_t length, vector<char> *out)
{
	out->resize(length);
	if (length > 0 && !pread_fully(fd, out->data(), length, offset)) {
		fprintf(stderr, "%s: short read at offset %llu (%llu bytes)\n", filename,
		        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length));
		return false;
	}
	return true;
}

bool decompress_stream(const vector<char> &compressed, string *output)
{
	ZSTD_DCtx *ctx = ZSTD_createDCtx();
	if (ctx == nullptr) {
		fprintf(stderr, "Failed to create ZSTD context\n");
		return false;
	}
	output->clear();
	ZSTD_inBuffer inbuf{ compressed.data(), compressed.size(), 0 };
	while (inbuf.pos < inbuf.size) {
		char buffer[8192];
		ZSTD_outBuffer outbuf{ buffer, sizeof(buffer), 0 };
		size_t ret = ZSTD_decompressStream(ctx, &outbuf, &inbuf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompressStream() failed: %s\n", ZSTD_getErrorName(ret));
			ZSTD_freeDCtx(ctx);
			return false;
		}
		output->append(buffer, outbuf.pos);
		if (ret == 0 && inbuf.pos == inbuf.size) {
			break;
		}
	}
	ZSTD_freeDCtx(ctx);
	return true;
}

bool decompress_block(const vector<char> &compressed, ZSTD_DCtx *ctx, ZSTD_DDict *ddict, string *output)
{
	ZSTD_DCtx_reset(ctx, ZSTD_reset_session_only);
	if (ddict != nullptr) {
		ZSTD_DCtx_refDDict(ctx, ddict);
	}
	unsigned long long content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
	    content_size > (1ULL << 30)) {
		fprintf(stderr, "Filename block has no usable content size\n");
		return false;
	}
	output->assign(content_size, '\0');
	size_t ret;
	if (ddict != nullptr) {
		ret = ZSTD_decompress_usingDDict(ctx, output->data(), output->size(), compressed.data(), compressed.size(), ddict);
	} else {
		ret = ZSTD_decompressDCtx(ctx, output->data(), output->size(), compressed.data(), compressed.size());
	}
	if (ZSTD_isError(ret)) {
		fprintf(stderr, "ZSTD_decompress() failed: %s\n", ZSTD_getErrorName(ret));
		return false;
	}
	output->resize(ret);
	return true;
}

// Filename blocks are NUL-separated, not NUL-terminated: the last path in a
// block has no trailing zero byte, so it must be picked up after the loop.
void extract_paths_from_block(const string &block, vector<string> *paths)
{
	const char *ptr = block.data();
	const char *end = ptr + block.size();
	while (ptr < end) {
		const char *term = static_cast<const char *>(memchr(ptr, '\0', end - ptr));
		if (term == nullptr) {
			term = end;
		}
		if (term != ptr) {
			paths->emplace_back(ptr, term - ptr);
		}
		ptr = term + 1;
	}
}

// The configuration block is a sequence of "key\0" followed by zero or more
// "value\0" entries and a terminating empty value (see gen_conf_block() in conf.cpp).
void parse_conf_block(const string &blob, PruneConfig *prune)
{
	const char *p = blob.data();
	const char *end = p + blob.size();
	auto next = [&p, end](string *out) {
		if (p >= end) {
			return false;
		}
		size_t len = strnlen(p, end - p);
		if (p + len >= end) {
			return false;
		}
		out->assign(p, len);
		p += len + 1;
		return true;
	};
	string key, value;
	while (next(&key)) {
		vector<string> values;
		while (next(&value) && !value.empty()) {
			values.push_back(value);
		}
		if (key == "prune_bind_mounts") {
			prune->prune_bind_mounts = !values.empty() && values[0] == "1";
		} else if (key == "prunefs") {
			prune->prunefs = values;
			for (string &fs : prune->prunefs) {
				for (char &c : fs) {
					c = toupper(static_cast<unsigned char>(c));
				}
			}
		} else if (key == "prunenames") {
			prune->prunenames = values;
		} else if (key == "prunepaths") {
			prune->prunepaths = values;
		}
	}
}

// updatedb never stores the scan root itself, only entries below it. The root
// is the directory part of the longest prefix shared by all (sorted) paths.
string scan_root_from_paths(const vector<string> &sorted_paths)
{
	if (sorted_paths.empty()) {
		return "/";
	}
	const string &a = sorted_paths.front();
	const string &b = sorted_paths.back();
	size_t n = 0;
	while (n < a.size() && n < b.size() && a[n] == b[n]) {
		++n;
	}
	size_t slash = a.rfind('/', n == 0 ? 0 : n - 1);
	if (slash == string::npos || slash == 0) {
		return "/";
	}
	return a.substr(0, slash);
}

bool load_snapshot(const char *filename, Snapshot *snapshot)
{
	int fd = open(filename, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		perror(filename);
		return false;
	}
	Header hdr;
	if (!read_header(fd, filename, &hdr)) {
		close(fd);
		return false;
	}
	if (hdr.metadata_length_bytes == 0) {
		fprintf(stderr, "%s: database does not contain metadata\n", filename);
		close(fd);
		return false;
	}

	vector<char> blob;
	string metadata_stream;
	if (!read_blob(fd, filename, hdr.metadata_offset_bytes, hdr.metadata_length_bytes, &blob) ||
	    !decompress_stream(blob, &metadata_stream)) {
		close(fd);
		return false;
	}
	const char *meta_ptr = metadata_stream.data();
	const char *meta_end = meta_ptr + metadata_stream.size();
	while (meta_ptr < meta_end) {
		FileMetadata meta;
		if (!decode_metadata_record(&meta_ptr, meta_end, &meta)) {
			fprintf(stderr, "%s: metadata stream is truncated after %zu entries\n", filename, snapshot->metadata.size());
			break;
		}
		if (meta.hash.kind != MetadataHashKind::None && snapshot->hash_kind == MetadataHashKind::None) {
			snapshot->hash_kind = meta.hash.kind;
		}
		snapshot->metadata.push_back(meta);
	}

	if (hdr.conf_block_length_bytes != 0) {
		if (!read_blob(fd, filename, hdr.conf_block_offset_bytes, hdr.conf_block_length_bytes, &blob)) {
			close(fd);
			return false;
		}
		parse_conf_block(string(blob.data(), blob.size()), &snapshot->prune);
	}

	const uint32_t num_blocks = hdr.num_docids;
	vector<uint64_t> offsets(static_cast<size_t>(num_blocks) + 1);
	if (!pread_fully(fd, offsets.data(), offsets.size() * sizeof(uint64_t), hdr.filename_index_offset_bytes)) {
		fprintf(stderr, "%s: failed reading filename index\n", filename);
		close(fd);
		return false;
	}

	ZSTD_DDict *ddict = nullptr;
	vector<char> dictionary;
	if (hdr.zstd_dictionary_length_bytes > 0) {
		if (!read_blob(fd, filename, hdr.zstd_dictionary_offset_bytes, hdr.zstd_dictionary_length_bytes, &dictionary)) {
			close(fd);
			return false;
		}
		ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
	}
	ZSTD_DCtx *ctx = ZSTD_createDCtx();
	bool ok = ctx != nullptr;
	struct stat sb;
	fstat(fd, &sb);
	string block_data;
	for (uint32_t block = 0; ok && block < num_blocks; ++block) {
		const uint64_t start = offsets[block];
		const uint64_t end = offsets[block + 1];
		if (end < start || !section_fits(start, end - start, sb.st_size)) {
			fprintf(stderr, "%s: corrupt filename index\n", filename);
			ok = false;
			break;
		}
		ok = read_blob(fd, filename, start, end - start, &blob) &&
			decompress_block(blob, ctx, ddict, &block_data);
		if (ok) {
			extract_paths_from_block(block_data, &snapshot->paths);
		}
	}
	if (ctx != nullptr) {
		ZSTD_freeDCtx(ctx);
	}
	if (ddict != nullptr) {
		ZSTD_freeDDict(ddict);
	}
	close(fd);
	if (!ok) {
		return false;
	}

	if (snapshot->paths.size() != snapshot->metadata.size()) {
		fprintf(stderr, "%s: mismatch between filenames (%zu) and metadata entries (%zu); "
		        "continuing with truncated metadata\n",
		        filename, snapshot->paths.size(), snapshot->metadata.size());
		snapshot->metadata.resize(snapshot->paths.size());
	}

	// Sort by path through an index permutation, so each string is moved once.
	vector<uint32_t> order(snapshot->paths.size());
	iota(order.begin(), order.end(), 0);
	sort(order.begin(), order.end(), [snapshot](uint32_t a, uint32_t b) {
		return snapshot->paths[a] < snapshot->paths[b];
	});
	vector<string> sorted_paths;
	vector<FileMetadata> sorted_metadata;
	sorted_paths.reserve(order.size());
	sorted_metadata.reserve(order.size());
	for (uint32_t i : order) {
		sorted_paths.push_back(std::move(snapshot->paths[i]));
		sorted_metadata.push_back(snapshot->metadata[i]);
	}
	snapshot->paths.swap(sorted_paths);
	snapshot->metadata.swap(sorted_metadata);
	snapshot->scan_root = scan_root_from_paths(snapshot->paths);
	return true;
}

bool load_history_events(const char *filename, vector<HistoryEvent> *events)
{
	int fd = open(filename, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		perror(filename);
		return false;
	}
	Header hdr;
	if (!read_header(fd, filename, &hdr)) {
		close(fd);
		return false;
	}
	if (hdr.history_length_bytes == 0) {
		fprintf(stderr, "%s: no history log available\n", filename);
		close(fd);
		return false;
	}
	vector<char> blob;
	string decompressed;
	bool ok = read_blob(fd, filename, hdr.history_offset_bytes, hdr.history_length_bytes, &blob) &&
		decompress_stream(blob, &decompressed);
	close(fd);
	if (!ok) {
		return false;
	}
	*events = decode_history_stream(decompressed);
	return true;
}

// ---------------------------------------------------------------------------
// Output

const char *hash_kind_name(MetadataHashKind kind)
{
	switch (kind) {
	case MetadataHashKind::None:
		return "none";
	case MetadataHashKind::Sha256:
		return "sha256";
	case MetadataHashKind::XxHash64:
		return "xxh64";
	}
	return "unknown";
}

string hash_hex(const MetadataHash &hash)
{
	static const char hex[] = "0123456789abcdef";
	string out;
	for (size_t i = 0; i < hash.length; ++i) {
		out.push_back(hex[(hash.value[i] >> 4) & 0xf]);
		out.push_back(hex[hash.value[i] & 0xf]);
	}
	return out;
}

string format_time(const dir_time &t)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%lld.%09d", static_cast<long long>(t.sec), t.nsec);
	return buf;
}

string format_metadata(const FileMetadata &meta)
{
	if (!meta.enabled) {
		return "(none)";
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "mode=%o uid=%u gid=%u size=%llu mtime=%s ctime=%s atime=%s",
	         unsigned(meta.mode), unsigned(meta.uid), unsigned(meta.gid),
	         static_cast<unsigned long long>(meta.size),
	         format_time(meta.mtime).c_str(), format_time(meta.ctime).c_str(), format_time(meta.atime).c_str());
	string out(buf);
	if (meta.hash.kind != MetadataHashKind::None && meta.hash.length != 0) {
		out += " hash=";
		out += hash_kind_name(meta.hash.kind);
		out += ':';
		out += hash_hex(meta.hash);
	}
	return out;
}

// Paths are byte strings; anything that is not a control character, a quote or
// a backslash is passed through unchanged (valid UTF-8 stays valid).
string json_string(const string &s)
{
	string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	out += '"';
	return out;
}

string json_metadata(const FileMetadata &meta)
{
	if (!meta.enabled) {
		return "null";
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"mode\":\"%o\",\"uid\":%u,\"gid\":%u,\"size\":%llu,",
	         unsigned(meta.mode), unsigned(meta.uid), unsigned(meta.gid), static_cast<unsigned long long>(meta.size));
	string out = buf;
	out += "\"mtime\":\"" + format_time(meta.mtime) + "\",";
	out += "\"ctime\":\"" + format_time(meta.ctime) + "\",";
	out += "\"atime\":\"" + format_time(meta.atime) + "\",";
	if (meta.hash.kind != MetadataHashKind::None && meta.hash.length != 0) {
		out += "\"hash\":{\"kind\":\"";
		out += hash_kind_name(meta.hash.kind);
		out += "\",\"hex\":\"" + hash_hex(meta.hash) + "\"}";
	} else {
		out += "\"hash\":null";
	}
	out += '}';
	return out;
}

string json_changed_fields(const FileMetadata &old_meta, const FileMetadata &new_meta)
{
	string fields = metadata_diff_fields(old_meta, new_meta);
	string out = "[";
	size_t start = 0;
	while (start <= fields.size() && !fields.empty()) {
		size_t comma = fields.find(',', start);
		string field = fields.substr(start, comma == string::npos ? string::npos : comma - start);
		if (out.size() > 1) {
			out += ',';
		}
		out += json_string(field);
		if (comma == string::npos) {
			break;
		}
		start = comma + 1;
	}
	out += ']';
	return out;
}

const char *kind_name(HistoryEventKind kind)
{
	switch (kind) {
	case HistoryEventKind::Added:
		return "ADDED";
	case HistoryEventKind::Removed:
		return "REMOVED";
	case HistoryEventKind::Modified:
		return "MODIFIED";
	case HistoryEventKind::RunMarker:
		return "RUN";
	}
	return "UNKNOWN";
}

void print_run_marker(const HistoryEvent &event)
{
	const bool baseline = event.path == "baseline";
	if (json_output) {
		printf("{\"kind\":\"RUN\",\"time\":\"%s\",\"baseline\":%s}\n",
		       format_time(event.event_time).c_str(), baseline ? "true" : "false");
		return;
	}
	char when[64] = "unknown time";
	time_t sec = event.event_time.sec;
	struct tm tm;
	if (sec != 0 && localtime_r(&sec, &tm) != nullptr) {
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", &tm);
	}
	printf("=== run %s%s ===\n", when, baseline ? " (baseline: full scan, no per-file events)" : "");
}

void print_event(const HistoryEvent &event)
{
	if (event.kind == HistoryEventKind::RunMarker) {
		print_run_marker(event);
		return;
	}
	if (json_output) {
		printf("{\"kind\":\"%s\",\"path\":%s,\"time\":\"%s\",\"old\":%s,\"new\":%s,\"changed\":%s}\n",
		       kind_name(event.kind), json_string(event.path).c_str(), format_time(event.event_time).c_str(),
		       json_metadata(event.old_metadata).c_str(), json_metadata(event.new_metadata).c_str(),
		       event.kind == HistoryEventKind::Modified ? json_changed_fields(event.old_metadata, event.new_metadata).c_str() : "[]");
		return;
	}
	printf("[%s] %s\n", kind_name(event.kind), event.path.c_str());
	if (event.kind == HistoryEventKind::Added) {
		printf("  new: %s\n", format_metadata(event.new_metadata).c_str());
	} else if (event.kind == HistoryEventKind::Removed) {
		printf("  old: %s\n", format_metadata(event.old_metadata).c_str());
	} else {
		printf("  old: %s\n", format_metadata(event.old_metadata).c_str());
		printf("  new: %s\n", format_metadata(event.new_metadata).c_str());
		printf("  changed: %s\n", metadata_diff_fields(event.old_metadata, event.new_metadata).c_str());
	}
}

size_t print_events(const vector<HistoryEvent> &events, const EventFilter &filter)
{
	size_t printed = 0;
	for (const HistoryEvent &event : events) {
		if (!filter.accepts(event.kind)) {
			continue;
		}
		print_event(event);
		if (event.kind != HistoryEventKind::RunMarker) {
			++printed;
		}
	}
	return printed;
}

// ---------------------------------------------------------------------------
// Comparisons

HistoryEvent make_event(HistoryEventKind kind, const string &path, const FileMetadata &old_meta, const FileMetadata &new_meta)
{
	HistoryEvent event;
	event.kind = kind;
	event.path = path;
	event.old_metadata = old_meta;
	event.new_metadata = new_meta;
	return event;
}

vector<HistoryEvent> diff_snapshots(const Snapshot &old_snap, const Snapshot &new_snap)
{
	vector<HistoryEvent> events;
	size_t i = 0, j = 0;
	while (i < old_snap.paths.size() || j < new_snap.paths.size()) {
		int cmp;
		if (i == old_snap.paths.size()) {
			cmp = 1;
		} else if (j == new_snap.paths.size()) {
			cmp = -1;
		} else {
			cmp = old_snap.paths[i].compare(new_snap.paths[j]);
		}
		if (cmp == 0) {
			if (!metadata_equals(old_snap.metadata[i], new_snap.metadata[j], compare_atime)) {
				events.push_back(make_event(HistoryEventKind::Modified, new_snap.paths[j], old_snap.metadata[i], new_snap.metadata[j]));
			}
			++i;
			++j;
		} else if (cmp < 0) {
			events.push_back(make_event(HistoryEventKind::Removed, old_snap.paths[i], old_snap.metadata[i], FileMetadata{}));
			++i;
		} else {
			events.push_back(make_event(HistoryEventKind::Added, new_snap.paths[j], FileMetadata{}, new_snap.metadata[j]));
			++j;
		}
	}
	return events;
}

FileMetadata metadata_from_stat(const struct stat &sb)
{
	FileMetadata meta;
	meta.enabled = true;
	meta.mode = sb.st_mode;
	meta.uid = sb.st_uid;
	meta.gid = sb.st_gid;
	meta.size = sb.st_size;
	meta.mtime = dir_time{ sb.st_mtim.tv_sec, int32_t(sb.st_mtim.tv_nsec) };
	meta.ctime = dir_time{ sb.st_ctim.tv_sec, int32_t(sb.st_ctim.tv_nsec) };
	meta.atime = dir_time{ sb.st_atim.tv_sec, int32_t(sb.st_atim.tv_nsec) };
	return meta;
}

// "" means the database paths are used as they are; otherwise ROOT is prepended.
string normalize_root(string root)
{
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	if (root == "/") {
		return "";
	}
	return root;
}

string join_path(const string &dir, const char *name)
{
	if (dir == "/") {
		return dir + name;
	}
	return dir + '/' + name;
}

// Filesystem type of the mount at REAL_PATH, upper-cased, or "" if unknown.
string mounted_fs_type(const string &real_path)
{
	FILE *f = setmntent("/proc/self/mounts", "r");
	if (f == nullptr) {
		return "";
	}
	string type;
	struct mntent *me;
	while ((me = getmntent(f)) != nullptr) {
		if (real_path == me->mnt_dir) {
			type = me->mnt_type;
			for (char &c : type) {
				c = toupper(static_cast<unsigned char>(c));
			}
		}
	}
	endmntent(f);
	return type;
}

struct WalkDir {
	string real;
	string db;
	dev_t dev;
	bool hash_allowed;
	bool check_fs;  // First directory of a new filesystem: decide about hashing.
};

// Walks the live tree once: entries found in the snapshot are compared (and
// marked), unknown entries are ADDED, and whatever was never seen is REMOVED.
// Pruning follows the configuration block stored in the database, except for
// bind mounts, which are not detected here.
bool diff_with_live(const Snapshot &snapshot, const string &root, vector<HistoryEvent> *events_out)
{
	const string normalized_root = normalize_root(root);
	const string real_root = normalized_root + snapshot.scan_root;
	vector<HistoryEvent> &events = *events_out;
	vector<bool> seen(snapshot.paths.size(), false);
	const MetadataHashKind hash_kind = snapshot.hash_kind;
	const PruneConfig &prune = snapshot.prune;

	struct stat root_sb;
	if (lstat(real_root.c_str(), &root_sb) != 0 || !S_ISDIR(root_sb.st_mode)) {
		fprintf(stderr, "%s: %s (database was built for %s)\n", real_root.c_str(), strerror(errno), snapshot.scan_root.c_str());
		return false;
	}

	vector<WalkDir> stack;
	stack.push_back(WalkDir{ real_root, snapshot.scan_root, root_sb.st_dev, true, true });
	while (!stack.empty()) {
		WalkDir dir = std::move(stack.back());
		stack.pop_back();
		DIR *dp = opendir(dir.real.c_str());
		if (dp == nullptr) {
			continue;
		}
		const int dfd = dirfd(dp);
		if (dir.check_fs && hash_kind != MetadataHashKind::None && dir.hash_allowed) {
			dir.hash_allowed = fs_allows_hashing(dfd, nullptr);
		}

		struct dirent *de;
		while ((de = readdir(dp)) != nullptr) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 || de->d_name[0] == '\0') {
				continue;
			}
			struct stat sb;
			if (fstatat(dfd, de->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
				continue;
			}
			const string db_path = join_path(dir.db, de->d_name);
			const string real_path = join_path(dir.real, de->d_name);
			FileMetadata live = metadata_from_stat(sb);
			if (hash_kind != MetadataHashKind::None && dir.hash_allowed && S_ISREG(sb.st_mode)) {
				if (!compute_file_hash_at(dfd, de->d_name, hash_kind, &live.hash)) {
					live.hash = MetadataHash{};
				}
			}

			auto it = lower_bound(snapshot.paths.begin(), snapshot.paths.end(), db_path);
			if (it != snapshot.paths.end() && *it == db_path) {
				const size_t idx = it - snapshot.paths.begin();
				seen[idx] = true;
				if (!metadata_equals(snapshot.metadata[idx], live, compare_atime)) {
					events.push_back(make_event(HistoryEventKind::Modified, db_path, snapshot.metadata[idx], live));
				}
			} else {
				events.push_back(make_event(HistoryEventKind::Added, db_path, FileMetadata{}, live));
			}

			if (!S_ISDIR(sb.st_mode)) {
				continue;
			}
			if (find(prune.prunenames.begin(), prune.prunenames.end(), de->d_name) != prune.prunenames.end() ||
			    find(prune.prunepaths.begin(), prune.prunepaths.end(), db_path) != prune.prunepaths.end()) {
				continue;
			}
			const bool new_fs = sb.st_dev != dir.dev;
			if (new_fs && !prune.prunefs.empty()) {
				const string type = mounted_fs_type(real_path);
				if (!type.empty() && find(prune.prunefs.begin(), prune.prunefs.end(), type) != prune.prunefs.end()) {
					continue;
				}
			}
			stack.push_back(WalkDir{ real_path, db_path, sb.st_dev, dir.hash_allowed, new_fs });
		}
		closedir(dp);
	}

	for (size_t i = 0; i < snapshot.paths.size(); ++i) {
		if (!seen[i]) {
			events.push_back(make_event(HistoryEventKind::Removed, snapshot.paths[i], snapshot.metadata[i], FileMetadata{}));
		}
	}
	sort(events.begin(), events.end(), [](const HistoryEvent &a, const HistoryEvent &b) { return a.path < b.path; });
	return true;
}

// ---------------------------------------------------------------------------

void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [--history] FLOCATE_DB\n", prog);
	fprintf(stderr, "       %s OLD_DB NEW_DB\n", prog);
	fprintf(stderr, "       %s --live ROOT FLOCATE_DB\n", prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "       --added-only     Show only ADDED events\n");
	fprintf(stderr, "       --removed-only   Show only REMOVED events\n");
	fprintf(stderr, "       --modified-only  Show only MODIFIED events\n");
	fprintf(stderr, "       --compare-atime  Treat atime-only differences as modifications\n");
	fprintf(stderr, "       --json           Print one JSON object per event\n");
	fprintf(stderr, "       --help           Show this help\n");
	fprintf(stderr, "  -V,  --version        Print version information\n");
}

void version()
{
	printf("flocate-showdiff %s\n", PACKAGE_VERSION);
	printf("Copyright (C) 2025 Robert Tulke\n");
	printf("License GPLv2+: GNU GPL version 2 or later <https://gnu.org/licenses/gpl.html>.\n");
	printf("This is free software: you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n");
}

bool run_history_mode(const char *db_path, const EventFilter &filter)
{
	vector<HistoryEvent> events;
	if (!load_history_events(db_path, &events)) {
		return false;
	}
	if (events.empty()) {
		if (!json_output) {
			printf("No history events recorded in %s.\n", db_path);
		}
		return true;
	}
	print_events(events, filter);
	return true;
}

bool run_diff_mode(const char *old_db, const char *new_db, const EventFilter &filter)
{
	Snapshot old_snap, new_snap;
	if (!load_snapshot(old_db, &old_snap) || !load_snapshot(new_db, &new_snap)) {
		return false;
	}
	vector<HistoryEvent> events = diff_snapshots(old_snap, new_snap);
	if (print_events(events, filter) == 0 && !json_output) {
		printf("No differences between %s and %s.\n", old_db, new_db);
	}
	return true;
}

bool run_live_mode(const char *db_path, const char *root, const EventFilter &filter)
{
	Snapshot snap;
	if (!load_snapshot(db_path, &snap)) {
		return false;
	}
	vector<HistoryEvent> events;
	if (!diff_with_live(snap, root, &events)) {
		return false;
	}
	if (print_events(events, filter) == 0 && !json_output) {
		printf("No differences between %s and live filesystem rooted at %s.\n", db_path, root);
	}
	return true;
}

}  // namespace

int main(int argc, char **argv)
{
	bool history_mode = false;
	const char *live_root = nullptr;
	EventFilter filter;
	vector<const char *> dbs;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--history") == 0) {
			history_mode = true;
		} else if (strcmp(argv[i], "--live") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			live_root = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		} else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
			version();
			return EXIT_SUCCESS;
		} else if (strcmp(argv[i], "--added-only") == 0) {
			filter.enable(HistoryEventKind::Added);
		} else if (strcmp(argv[i], "--removed-only") == 0) {
			filter.enable(HistoryEventKind::Removed);
		} else if (strcmp(argv[i], "--modified-only") == 0) {
			filter.enable(HistoryEventKind::Modified);
		} else if (strcmp(argv[i], "--compare-atime") == 0) {
			compare_atime = true;
		} else if (strcmp(argv[i], "--json") == 0) {
			json_output = true;
		} else if (argv[i][0] == '-' && argv[i][1] != '\0') {
			fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
			usage(argv[0]);
			return EXIT_FAILURE;
		} else {
			dbs.push_back(argv[i]);
		}
	}

	if (dbs.empty() || (history_mode && live_root != nullptr)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (live_root != nullptr) {
		if (dbs.size() != 1) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		return run_live_mode(dbs[0], live_root, filter) ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (dbs.size() == 1) {
		return run_history_mode(dbs[0], filter) ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (dbs.size() == 2 && !history_mode) {
		return run_diff_mode(dbs[0], dbs[1], filter) ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	usage(argv[0]);
	return EXIT_FAILURE;
}
