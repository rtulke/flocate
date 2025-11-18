#include "db.h"
#include "metadata.h"
#include "metadata_hash.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zstd.h>

using namespace std;

namespace {

struct Snapshot {
	vector<string> paths;
	vector<FileMetadata> metadata;
	MetadataHashKind hash_kind = MetadataHashKind::None;
};

bool pread_fully(int fd, void *buf, size_t len, off_t offset)
{
	char *ptr = static_cast<char *>(buf);
	while (len > 0) {
		ssize_t ret = pread(fd, ptr, len, offset);
		if (ret <= 0) {
			return false;
		}
		ptr += ret;
		len -= ret;
		offset += ret;
	}
	return true;
}

vector<char> read_blob(int fd, uint64_t offset, uint64_t length)
{
	vector<char> buf(length);
	if (length > 0 && !pread_fully(fd, buf.data(), length, offset)) {
		fprintf(stderr, "Failed to read blob at offset %llu length %llu\n",
		        static_cast<unsigned long long>(offset),
		        static_cast<unsigned long long>(length));
		exit(1);
	}
	return buf;
}

string decompress_stream(const vector<char> &compressed)
{
	ZSTD_DCtx *ctx = ZSTD_createDCtx();
	if (ctx == nullptr) {
		fprintf(stderr, "Failed to create ZSTD context\n");
		exit(1);
	}

	string output;
	ZSTD_inBuffer inbuf{ compressed.data(), compressed.size(), 0 };
	while (inbuf.pos < inbuf.size) {
		char buffer[8192];
		ZSTD_outBuffer outbuf{ buffer, sizeof(buffer), 0 };
		size_t ret = ZSTD_decompressStream(ctx, &outbuf, &inbuf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompressStream() failed: %s\n", ZSTD_getErrorName(ret));
			exit(1);
		}
		output.append(buffer, outbuf.pos);
		if (ret == 0 && inbuf.pos == inbuf.size) {
			break;
		}
	}
	ZSTD_freeDCtx(ctx);
	return output;
}

bool decode_metadata(const char *&ptr, const char *end, FileMetadata &meta)
{
	if (ptr >= end) {
		return false;
	}
	unsigned char enabled = static_cast<unsigned char>(*ptr++);
	if (enabled == 0) {
		meta = FileMetadata{};
		return true;
	}

	const size_t fixed = sizeof(meta.mode) + sizeof(meta.uid) + sizeof(meta.gid) + sizeof(meta.size) +
		sizeof(meta.mtime.sec) + sizeof(meta.mtime.nsec) +
		sizeof(meta.ctime.sec) + sizeof(meta.ctime.nsec) +
		sizeof(meta.atime.sec) + sizeof(meta.atime.nsec);
	if (static_cast<size_t>(end - ptr) < fixed + 2) {
		return false;
	}

	memcpy(&meta.mode, ptr, sizeof(meta.mode));
	ptr += sizeof(meta.mode);
	memcpy(&meta.uid, ptr, sizeof(meta.uid));
	ptr += sizeof(meta.uid);
	memcpy(&meta.gid, ptr, sizeof(meta.gid));
	ptr += sizeof(meta.gid);
	memcpy(&meta.size, ptr, sizeof(meta.size));
	ptr += sizeof(meta.size);
	memcpy(&meta.mtime.sec, ptr, sizeof(meta.mtime.sec));
	ptr += sizeof(meta.mtime.sec);
	memcpy(&meta.mtime.nsec, ptr, sizeof(meta.mtime.nsec));
	ptr += sizeof(meta.mtime.nsec);
	memcpy(&meta.ctime.sec, ptr, sizeof(meta.ctime.sec));
	ptr += sizeof(meta.ctime.sec);
	memcpy(&meta.ctime.nsec, ptr, sizeof(meta.ctime.nsec));
	ptr += sizeof(meta.ctime.nsec);
	memcpy(&meta.atime.sec, ptr, sizeof(meta.atime.sec));
	ptr += sizeof(meta.atime.sec);
	memcpy(&meta.atime.nsec, ptr, sizeof(meta.atime.nsec));
	ptr += sizeof(meta.atime.nsec);
	meta.enabled = true;

	meta.hash.kind = static_cast<MetadataHashKind>(*ptr++);
	uint8_t hash_len = static_cast<uint8_t>(*ptr++);
	if (static_cast<size_t>(end - ptr) < hash_len) {
		return false;
	}
	meta.hash.length = min<size_t>(hash_len, meta.hash.value.size());
	if (meta.hash.length > 0) {
		memcpy(meta.hash.value.data(), ptr, meta.hash.length);
	}
	ptr += hash_len;
	return true;
}

string format_metadata(const FileMetadata &meta)
{
	if (!meta.enabled) {
		return "(none)";
	}
	char buf[512];
	snprintf(buf, sizeof(buf), "mode=%o uid=%u gid=%u size=%llu mtime=%lld.%09d",
	         unsigned(meta.mode), unsigned(meta.uid), unsigned(meta.gid),
	         static_cast<unsigned long long>(meta.size),
	         static_cast<long long>(meta.mtime.sec), meta.mtime.nsec);
	string out(buf);
	if (meta.hash.kind != MetadataHashKind::None && meta.hash.length != 0) {
		out += " hash=";
		out += (meta.hash.kind == MetadataHashKind::Sha256) ? "sha256" :
		       (meta.hash.kind == MetadataHashKind::XxHash64) ? "xxh64" : "unknown";
		out += ':';
		static const char hex[] = "0123456789abcdef";
		for (size_t i = 0; i < meta.hash.length; ++i) {
			out.push_back(hex[(meta.hash.value[i] >> 4) & 0xf]);
			out.push_back(hex[meta.hash.value[i] & 0xf]);
		}
	}
	return out;
}

void print_event(const HistoryEvent &event)
{
	const char *kind = "";
	switch (event.kind) {
	case HistoryEventKind::Added:
		kind = "ADDED";
		break;
	case HistoryEventKind::Removed:
		kind = "REMOVED";
		break;
	case HistoryEventKind::Modified:
		kind = "MODIFIED";
		break;
	case HistoryEventKind::RunMarker:
		return;
	}
	printf("[%s] %s\n", kind, event.path.c_str());
	if (event.kind == HistoryEventKind::Added) {
		printf("  new: %s\n", format_metadata(event.new_metadata).c_str());
	} else if (event.kind == HistoryEventKind::Removed) {
		printf("  old: %s\n", format_metadata(event.old_metadata).c_str());
	} else {
		printf("  old: %s\n", format_metadata(event.old_metadata).c_str());
		printf("  new: %s\n", format_metadata(event.new_metadata).c_str());
	}
}

string decompress_block(const vector<char> &compressed, ZSTD_DCtx *ctx, ZSTD_DDict *ddict)
{
	ZSTD_DCtx_reset(ctx, ZSTD_reset_session_only);
	if (ddict != nullptr) {
		ZSTD_DCtx_refDDict(ctx, ddict);
	}

	unsigned long long content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (content_size != ZSTD_CONTENTSIZE_ERROR && content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
		string output(content_size, '\0');
		size_t ret;
		if (ddict != nullptr) {
			ret = ZSTD_decompress_usingDDict(ctx, output.data(), output.size(), compressed.data(), compressed.size(), ddict);
		} else {
			ret = ZSTD_decompressDCtx(ctx, output.data(), output.size(), compressed.data(), compressed.size());
		}
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompress() failed: %s\n", ZSTD_getErrorName(ret));
			exit(1);
		}
		output.resize(ret);
		return output;
	}

	string output;
	ZSTD_inBuffer inbuf{ compressed.data(), compressed.size(), 0 };
	while (inbuf.pos < inbuf.size) {
		char buffer[4096];
		ZSTD_outBuffer outbuf{ buffer, sizeof(buffer), 0 };
		size_t ret = ZSTD_decompressStream(ctx, &outbuf, &inbuf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompressStream() failed: %s\n", ZSTD_getErrorName(ret));
			exit(1);
		}
		output.append(buffer, outbuf.pos);
		if (ret == 0 && inbuf.pos == inbuf.size) {
			break;
		}
	}
	return output;
}

void extract_paths_from_block(const string &block, vector<string> *paths)
{
	const char *ptr = block.data();
	const char *end = ptr + block.size();
	while (ptr < end) {
		const char *term = static_cast<const char *>(memchr(ptr, '\0', end - ptr));
		if (term == nullptr) {
			break;
		}
		if (term != ptr) {
			paths->emplace_back(ptr, term - ptr);
		}
		ptr = term + 1;
	}
}

bool load_snapshot(const char *filename, Snapshot *snapshot)
{
	int fd = open(filename, O_RDONLY);
	if (fd == -1) {
		perror(filename);
		return false;
	}

	Header hdr;
	if (!pread_fully(fd, &hdr, sizeof(hdr), 0) || memcmp(hdr.magic, "\0plocate", 8) != 0) {
		fprintf(stderr, "%s: invalid or corrupt database\n", filename);
		close(fd);
		return false;
	}
	if (hdr.metadata_length_bytes == 0) {
		fprintf(stderr, "%s: database does not contain metadata\n", filename);
		close(fd);
		return false;
	}

	vector<char> metadata_blob = read_blob(fd, hdr.metadata_offset_bytes, hdr.metadata_length_bytes);
	string metadata_stream = decompress_stream(metadata_blob);
	const char *meta_ptr = metadata_stream.data();
	const char *meta_end = meta_ptr + metadata_stream.size();
	while (meta_ptr < meta_end) {
		FileMetadata meta;
		if (!decode_metadata(meta_ptr, meta_end, meta)) {
			break;
		}
		if (meta.hash.kind != MetadataHashKind::None && snapshot->hash_kind == MetadataHashKind::None) {
			snapshot->hash_kind = meta.hash.kind;
		}
		snapshot->metadata.push_back(meta);
	}

	uint32_t num_blocks = hdr.num_docids;
	vector<uint64_t> offsets(num_blocks + 1);
	size_t offsets_bytes = static_cast<size_t>(num_blocks + 1) * sizeof(uint64_t);
	if (!pread_fully(fd, offsets.data(), offsets_bytes, hdr.filename_index_offset_bytes)) {
		fprintf(stderr, "%s: failed reading filename index\n", filename);
		close(fd);
		return false;
	}

	ZSTD_DDict *ddict = nullptr;
	vector<char> dictionary;
	if (hdr.zstd_dictionary_length_bytes > 0) {
		dictionary = read_blob(fd, hdr.zstd_dictionary_offset_bytes, hdr.zstd_dictionary_length_bytes);
		ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
		if (ddict == nullptr) {
			fprintf(stderr, "%s: failed creating ZSTD dictionary\n", filename);
			close(fd);
			return false;
		}
	}

	ZSTD_DCtx *ctx = ZSTD_createDCtx();
	if (ctx == nullptr) {
		fprintf(stderr, "Failed to create ZSTD context\n");
		if (ddict != nullptr)
			ZSTD_freeDDict(ddict);
		close(fd);
		return false;
	}

	for (uint32_t block = 0; block < num_blocks; ++block) {
		uint64_t start = offsets[block];
		uint64_t end = offsets[block + 1];
		if (end < start) {
			fprintf(stderr, "%s: corrupt filename index\n", filename);
			ZSTD_freeDCtx(ctx);
			if (ddict != nullptr)
				ZSTD_freeDDict(ddict);
			close(fd);
			return false;
		}
		vector<char> compressed = read_blob(fd, start, end - start);
		string block_data = decompress_block(compressed, ctx, ddict);
		extract_paths_from_block(block_data, &snapshot->paths);
	}

	ZSTD_freeDCtx(ctx);
	if (ddict != nullptr)
		ZSTD_freeDDict(ddict);
	close(fd);

	if (snapshot->paths.size() != snapshot->metadata.size()) {
		fprintf(stderr, "%s: mismatch between filenames (%zu) and metadata entries (%zu)\n",
		        filename, snapshot->paths.size(), snapshot->metadata.size());
		return false;
	}

	vector<pair<string, FileMetadata>> merged;
	merged.reserve(snapshot->paths.size());
	for (size_t i = 0; i < snapshot->paths.size(); ++i) {
		merged.emplace_back(snapshot->paths[i], snapshot->metadata[i]);
	}
	sort(merged.begin(), merged.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	snapshot->paths.clear();
	snapshot->metadata.clear();
	for (const auto &entry : merged) {
		snapshot->paths.push_back(entry.first);
		snapshot->metadata.push_back(entry.second);
	}
	return true;
}

vector<HistoryEvent> parse_history(const string &data)
{
	vector<HistoryEvent> events;
	const char *ptr = data.data();
	const char *end = ptr + data.size();
	while (ptr < end) {
		if (static_cast<size_t>(end - ptr) < 1 + sizeof(int64_t) + sizeof(int32_t) + sizeof(uint32_t)) {
			break;
		}
		HistoryEvent event;
		event.kind = static_cast<HistoryEventKind>(*ptr++);
		memcpy(&event.event_time.sec, ptr, sizeof(event.event_time.sec));
		ptr += sizeof(event.event_time.sec);
		memcpy(&event.event_time.nsec, ptr, sizeof(event.event_time.nsec));
		ptr += sizeof(event.event_time.nsec);
		uint32_t path_len;
		memcpy(&path_len, ptr, sizeof(path_len));
		ptr += sizeof(path_len);
		if (static_cast<size_t>(end - ptr) < path_len) {
			break;
		}
		event.path.assign(ptr, path_len);
		ptr += path_len;
		if (!decode_metadata(ptr, end, event.old_metadata)) {
			break;
		}
		if (!decode_metadata(ptr, end, event.new_metadata)) {
			break;
		}
		events.push_back(move(event));
	}
	return events;
}

bool load_history_events(const char *filename, vector<HistoryEvent> *events)
{
	int fd = open(filename, O_RDONLY);
	if (fd == -1) {
		perror(filename);
		return false;
	}

	Header hdr;
	if (!pread_fully(fd, &hdr, sizeof(hdr), 0) || memcmp(hdr.magic, "\0plocate", 8) != 0) {
		fprintf(stderr, "%s: invalid or corrupt database\n", filename);
		close(fd);
		return false;
	}
	if (hdr.history_length_bytes == 0 || hdr.max_version < 3) {
		fprintf(stderr, "%s: no history log available\n", filename);
		close(fd);
		return false;
	}

	vector<char> history_blob = read_blob(fd, hdr.history_offset_bytes, hdr.history_length_bytes);
	close(fd);

	string decompressed = decompress_stream(history_blob);
	*events = parse_history(decompressed);
	return true;
}

vector<HistoryEvent> diff_snapshots(const Snapshot &old_snap, const Snapshot &new_snap)
{
	vector<HistoryEvent> events;
	size_t i = 0, j = 0;
	while (i < old_snap.paths.size() && j < new_snap.paths.size()) {
		if (old_snap.paths[i] == new_snap.paths[j]) {
			if (!metadata_equals(old_snap.metadata[i], new_snap.metadata[j])) {
				HistoryEvent event;
				event.kind = HistoryEventKind::Modified;
				event.path = new_snap.paths[j];
				event.old_metadata = old_snap.metadata[i];
				event.new_metadata = new_snap.metadata[j];
				events.push_back(move(event));
			}
			++i;
			++j;
		} else if (old_snap.paths[i] < new_snap.paths[j]) {
			HistoryEvent event;
			event.kind = HistoryEventKind::Removed;
			event.path = old_snap.paths[i];
			event.old_metadata = old_snap.metadata[i];
			events.push_back(move(event));
			++i;
		} else {
			HistoryEvent event;
			event.kind = HistoryEventKind::Added;
			event.path = new_snap.paths[j];
			event.new_metadata = new_snap.metadata[j];
			events.push_back(move(event));
			++j;
		}
	}
	while (i < old_snap.paths.size()) {
		HistoryEvent event;
		event.kind = HistoryEventKind::Removed;
		event.path = old_snap.paths[i];
		event.old_metadata = old_snap.metadata[i];
		events.push_back(move(event));
		++i;
	}
	while (j < new_snap.paths.size()) {
		HistoryEvent event;
		event.kind = HistoryEventKind::Added;
		event.path = new_snap.paths[j];
		event.new_metadata = new_snap.metadata[j];
		events.push_back(move(event));
		++j;
	}
	return events;
}

vector<HistoryEvent> diff_with_live(const Snapshot &snapshot, const string &root)
{
	string normalized_root = normalize_root(root);
	unordered_map<string, size_t> index;
	index.reserve(snapshot.paths.size() * 2);
	for (size_t i = 0; i < snapshot.paths.size(); ++i) {
		index.emplace(snapshot.paths[i], i);
	}
	vector<HistoryEvent> events;
	MetadataHashKind hash_kind = snapshot.hash_kind;

	for (size_t i = 0; i < snapshot.paths.size(); ++i) {
		const string &db_path = snapshot.paths[i];
		const FileMetadata &db_meta = snapshot.metadata[i];
		string real_path = map_db_to_real(normalized_root, db_path);
		struct stat sb;
		if (lstat(real_path.c_str(), &sb) != 0) {
			HistoryEvent event;
			event.kind = HistoryEventKind::Removed;
			event.path = db_path;
			event.old_metadata = db_meta;
			events.push_back(move(event));
			continue;
		}
		FileMetadata live_meta = metadata_from_stat(sb);
		if (hash_kind != MetadataHashKind::None && S_ISREG(sb.st_mode)) {
			if (!compute_path_hash(real_path, hash_kind, &live_meta.hash)) {
				live_meta.hash = MetadataHash{};
			}
		}
		if (!metadata_equals(db_meta, live_meta)) {
			HistoryEvent event;
			event.kind = HistoryEventKind::Modified;
			event.path = db_path;
			event.old_metadata = db_meta;
			event.new_metadata = live_meta;
			events.push_back(move(event));
		}
	}

	vector<string> stack;
	stack.push_back(normalized_root.empty() ? "/" : normalized_root);
	unordered_set<string> visited;
	while (!stack.empty()) {
		string dir = move(stack.back());
		stack.pop_back();
		if (!visited.insert(dir).second) {
			continue;
		}
		DIR *dp = opendir(dir.c_str());
		if (dp == nullptr) {
			continue;
		}
		struct dirent *de;
		while ((de = readdir(dp)) != nullptr) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			string child = dir;
			if (!(child.size() == 1 && child[0] == '/')) {
				child.push_back('/');
			}
			child += de->d_name;
			struct stat sb;
			if (lstat(child.c_str(), &sb) != 0) {
				continue;
			}
			string db_path = map_real_to_db(normalized_root, child);
			if (db_path.empty()) {
				continue;
			}
			auto it = index.find(db_path);
			if (it == index.end()) {
				HistoryEvent event;
				event.kind = HistoryEventKind::Added;
				event.path = db_path;
				event.new_metadata = metadata_from_stat(sb);
				if (hash_kind != MetadataHashKind::None && S_ISREG(sb.st_mode)) {
					if (!compute_path_hash(child, hash_kind, &event.new_metadata.hash)) {
						event.new_metadata.hash = MetadataHash{};
					}
				}
				events.push_back(move(event));
			}
			if (S_ISDIR(sb.st_mode) && !S_ISLNK(sb.st_mode)) {
				stack.push_back(child);
			}
		}
		closedir(dp);
	}
	return events;
}

void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [--history] PLOCATE_DB\n", prog);
	fprintf(stderr, "       %s OLD_DB NEW_DB\n", prog);
	fprintf(stderr, "       %s --live ROOT PLOCATE_DB\n", prog);
}

bool run_history_mode(const char *db_path)
{
	vector<HistoryEvent> events;
	if (!load_history_events(db_path, &events)) {
		return false;
	}
	if (events.empty()) {
		printf("No history events recorded in %s.\n", db_path);
		return true;
	}
	for (const HistoryEvent &event : events) {
		print_event(event);
	}
	return true;
}

bool run_diff_mode(const char *old_db, const char *new_db)
{
	Snapshot old_snap, new_snap;
	if (!load_snapshot(old_db, &old_snap) || !load_snapshot(new_db, &new_snap)) {
		return false;
	}
	vector<HistoryEvent> events = diff_snapshots(old_snap, new_snap);
	if (events.empty()) {
		printf("No differences between %s and %s.\n", old_db, new_db);
		return true;
	}
	for (const HistoryEvent &event : events) {
		print_event(event);
	}
	return true;
}

bool run_live_mode(const char *db_path, const char *root)
{
	Snapshot snap;
	if (!load_snapshot(db_path, &snap)) {
		return false;
	}
	vector<HistoryEvent> events = diff_with_live(snap, root);
	if (events.empty()) {
		printf("No differences between %s and live filesystem rooted at %s.\n", db_path, root);
		return true;
	}
	for (const HistoryEvent &event : events) {
		print_event(event);
	}
	return true;
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

bool compute_path_hash(const string &path, MetadataHashKind kind, MetadataHash *out)
{
	if (kind == MetadataHashKind::None || out == nullptr) {
		if (out != nullptr) {
			*out = MetadataHash{};
		}
		return true;
	}
	MetadataHashBuilder builder(kind);
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		return false;
	}
	vector<char> buffer(1 << 15);
	while (true) {
		ssize_t ret = read(fd, buffer.data(), buffer.size());
		if (ret == 0) {
			break;
		}
		if (ret < 0) {
			int saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return false;
		}
		builder.update(buffer.data(), static_cast<size_t>(ret));
	}
	close(fd);
	*out = builder.finalize();
	return true;
}

string normalize_root(string root)
{
	if (root.empty() || root == "/") {
		return "";
	}
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	return root;
}

string map_db_to_real(const string &root, const string &db_path)
{
	if (root.empty() || db_path.empty() || db_path[0] != '/') {
		return db_path;
	}
	return root + db_path;
}

string map_real_to_db(const string &root, const string &real_path)
{
	if (root.empty()) {
		return real_path;
	}
	if (real_path.compare(0, root.size(), root) != 0) {
		return "";
	}
	string suffix = real_path.substr(root.size());
	if (suffix.empty()) {
		return "/";
	}
	if (suffix[0] != '/') {
		suffix.insert(suffix.begin(), '/');
	}
	return suffix;
}

}  // namespace

int main(int argc, char **argv)
{
	bool history_mode = false;
	const char *live_root = nullptr;
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
		} else {
			dbs.push_back(argv[i]);
		}
	}

	if (dbs.empty()) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (history_mode && live_root != nullptr) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (live_root != nullptr) {
		if (dbs.size() != 1) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		return run_live_mode(dbs[0], live_root) ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (history_mode || dbs.size() == 1) {
		if (dbs.size() != 1) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		return run_history_mode(dbs[0]) ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (dbs.size() == 2) {
		return run_diff_mode(dbs[0], dbs[1]) ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	usage(argv[0]);
	return EXIT_FAILURE;
}
