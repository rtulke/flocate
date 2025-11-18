#ifndef METADATA_H
#define METADATA_H 1

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <sys/types.h>

struct dir_time {
	int64_t sec;
	int32_t nsec;

	bool operator<(const dir_time &other) const
	{
		if (sec != other.sec)
			return sec < other.sec;
		return nsec < other.nsec;
	}
	bool operator>=(const dir_time &other) const
	{
		return !(other < *this);
	}
};
constexpr dir_time unknown_dir_time{ 0, 0 };
constexpr dir_time not_a_dir{ -1, 0 };

enum class MetadataHashKind : uint8_t {
	None = 0,
	Sha256 = 1,
	XxHash64 = 2,
};

struct MetadataHash {
	MetadataHashKind kind = MetadataHashKind::None;
	std::array<uint8_t, 32> value{};
	size_t length = 0;  // Useful for hashes shorter than 32 bytes.
};

struct FileMetadata {
	bool enabled = false;
	mode_t mode = 0;
	uid_t uid = 0;
	gid_t gid = 0;
	uint64_t size = 0;
	dir_time mtime = unknown_dir_time;
	dir_time ctime = unknown_dir_time;
	dir_time atime = unknown_dir_time;
	MetadataHash hash;
};

struct FileRecord {
	std::string path;
	dir_time dir_timestamp = unknown_dir_time;
	FileMetadata metadata;
};

enum class HistoryEventKind : uint8_t {
	Added = 0,
	Removed = 1,
	Modified = 2,
};

struct HistoryEvent {
	HistoryEventKind kind = HistoryEventKind::Added;
	std::string path;
	dir_time event_time = unknown_dir_time;
	FileMetadata old_metadata;
	FileMetadata new_metadata;
};

inline bool metadata_equals(const FileMetadata &a, const FileMetadata &b)
{
	if (a.enabled != b.enabled)
		return false;
	if (!a.enabled)
		return true;
	return a.mode == b.mode &&
	       a.uid == b.uid &&
	       a.gid == b.gid &&
	       a.size == b.size &&
	       a.mtime.sec == b.mtime.sec &&
	       a.mtime.nsec == b.mtime.nsec &&
	       a.ctime.sec == b.ctime.sec &&
	       a.ctime.nsec == b.ctime.nsec &&
	       a.atime.sec == b.atime.sec &&
	       a.atime.nsec == b.atime.nsec &&
	       a.hash.kind == b.hash.kind &&
	       a.hash.length == b.hash.length &&
	       std::equal(a.hash.value.begin(), a.hash.value.begin() + a.hash.length,
	                  b.hash.value.begin());
}

#endif  // METADATA_H
