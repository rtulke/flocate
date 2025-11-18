#ifndef METADATA_H
#define METADATA_H 1

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

#endif  // METADATA_H
