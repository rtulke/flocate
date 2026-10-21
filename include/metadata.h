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
	RunMarker = 3,
};

struct HistoryEvent {
	HistoryEventKind kind = HistoryEventKind::Added;
	std::string path;
	dir_time event_time = unknown_dir_time;
	FileMetadata old_metadata;
	FileMetadata new_metadata;
};

inline bool dir_time_equals(const dir_time &a, const dir_time &b)
{
	return a.sec == b.sec && a.nsec == b.nsec;
}

inline bool hash_equals(const MetadataHash &a, const MetadataHash &b)
{
	return a.kind == b.kind &&
	       a.length == b.length &&
	       std::equal(a.value.begin(), a.value.begin() + a.length, b.value.begin());
}

// atime is excluded by default: with relatime (the usual mount default) it is
// bumped by any reader at least once a day, including our own hashing pass,
// which would flag every file as modified on every run.
inline bool metadata_equals(const FileMetadata &a, const FileMetadata &b, bool compare_atime = false)
{
	if (a.enabled != b.enabled)
		return false;
	if (!a.enabled)
		return true;
	if (compare_atime && !dir_time_equals(a.atime, b.atime))
		return false;
	return a.mode == b.mode &&
	       a.uid == b.uid &&
	       a.gid == b.gid &&
	       a.size == b.size &&
	       dir_time_equals(a.mtime, b.mtime) &&
	       dir_time_equals(a.ctime, b.ctime) &&
	       hash_equals(a.hash, b.hash);
}

// Comma-separated names of all fields that differ (atime included), for diagnostics.
inline std::string metadata_diff_fields(const FileMetadata &a, const FileMetadata &b)
{
	std::string out;
	auto add = [&out](const char *name) {
		if (!out.empty())
			out += ',';
		out += name;
	};
	if (a.enabled != b.enabled) {
		add(a.enabled ? "metadata-lost" : "metadata-gained");
		return out;
	}
	if (!a.enabled)
		return out;
	if (a.mode != b.mode)
		add("mode");
	if (a.uid != b.uid)
		add("uid");
	if (a.gid != b.gid)
		add("gid");
	if (a.size != b.size)
		add("size");
	if (!dir_time_equals(a.mtime, b.mtime))
		add("mtime");
	if (!dir_time_equals(a.ctime, b.ctime))
		add("ctime");
	if (!dir_time_equals(a.atime, b.atime))
		add("atime");
	if (!hash_equals(a.hash, b.hash))
		add("hash");
	return out;
}

#endif  // METADATA_H
