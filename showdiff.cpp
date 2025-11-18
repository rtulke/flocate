#include "db.h"
#include "metadata.h"

#include <algorithm>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <zstd.h>

using namespace std;

namespace {

bool pread_all(int fd, void *buf, size_t len, off_t offset)
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

string decompress_history(const vector<char> &compressed)
{
	ZSTD_DCtx *ctx = ZSTD_createDCtx();
	if (ctx == nullptr) {
		fprintf(stderr, "Failed to create ZSTD context\n");
		exit(1);
	}

	string output;
	ZSTD_inBuffer inbuf;
	inbuf.src = compressed.data();
	inbuf.size = compressed.size();
	inbuf.pos = 0;

	while (inbuf.pos < inbuf.size) {
		char buffer[4096];
		ZSTD_outBuffer outbuf;
		outbuf.dst = buffer;
		outbuf.size = sizeof(buffer);
		outbuf.pos = 0;

		size_t ret = ZSTD_decompressStream(ctx, &outbuf, &inbuf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompressStream() failed: %s\n", ZSTD_getErrorName(ret));
			exit(1);
		}
		output.append(buffer, outbuf.pos);
		if (ret == 0 && inbuf.pos == inbuf.size) {
			break;  // End of frame.
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
	const size_t fixed_size = sizeof(meta.mode) + sizeof(meta.uid) + sizeof(meta.gid) + sizeof(meta.size) +
		sizeof(meta.mtime.sec) + sizeof(meta.mtime.nsec) +
		sizeof(meta.ctime.sec) + sizeof(meta.ctime.nsec) +
		sizeof(meta.atime.sec) + sizeof(meta.atime.nsec);
	if (end - ptr < static_cast<ptrdiff_t>(fixed_size + 2)) {
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
	if (end - ptr < hash_len) {
		return false;
	}
	meta.hash.length = min<size_t>(hash_len, meta.hash.value.size());
	if (meta.hash.length != 0) {
		memcpy(meta.hash.value.data(), ptr, meta.hash.length);
	}
	ptr += hash_len;
	return true;
}

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
		out += hash_kind_name(meta.hash.kind);
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

vector<HistoryEvent> parse_history(const string &data)
{
	vector<HistoryEvent> events;
	const char *ptr = data.data();
	const char *end = ptr + data.size();
	while (ptr < end) {
		if (end - ptr < 1 + sizeof(int64_t) + sizeof(int32_t) + sizeof(uint32_t)) {
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
		if (end - ptr < path_len) {
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

void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s PLOCATE_DB\n", prog);
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	const char *path = argv[1];
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		perror(path);
		return EXIT_FAILURE;
	}

	Header hdr;
	if (!pread_all(fd, &hdr, sizeof(hdr), 0)) {
		fprintf(stderr, "%s: failed to read header\n", path);
		close(fd);
		return EXIT_FAILURE;
	}
	if (memcmp(hdr.magic, "\0plocate", 8) != 0) {
		fprintf(stderr, "%s: not a plocate database\n", path);
		close(fd);
		return EXIT_FAILURE;
	}
	if (hdr.version < 1 || hdr.max_version < 3) {
		fprintf(stderr, "%s: database does not contain history data (version=%u max_version=%u)\n",
		        path, hdr.version, hdr.max_version);
		close(fd);
		return EXIT_FAILURE;
	}
	if (hdr.history_length_bytes == 0) {
		fprintf(stderr, "%s: history log not found in this database\n", path);
		close(fd);
		return EXIT_FAILURE;
	}

	vector<char> compressed(hdr.history_length_bytes);
	if (!pread_all(fd, compressed.data(), compressed.size(), hdr.history_offset_bytes)) {
		fprintf(stderr, "%s: failed reading history log\n", path);
		close(fd);
		return EXIT_FAILURE;
	}
	close(fd);

	string decompressed = decompress_history(compressed);
	vector<HistoryEvent> events = parse_history(decompressed);
	if (events.empty()) {
		printf("History log is empty.\n");
		return EXIT_SUCCESS;
	}
	for (const HistoryEvent &event : events) {
		print_event(event);
	}
	return EXIT_SUCCESS;
}
