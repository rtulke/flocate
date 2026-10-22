#ifndef METADATA_CODEC_H
#define METADATA_CODEC_H 1

#include "metadata.h"

#include <stddef.h>
#include <string>
#include <vector>

// Serialisation of FileMetadata and HistoryEvent, shared by updatedb (writer and
// incremental reader of the previous database) and flocate-showdiff.
//
// All integers are stored little-endian regardless of host byte order, so a
// database can be examined on a different machine than it was created on.
//
// Metadata record:
//   u8  enabled            0 ends the record (no metadata for this entry)
//   u32 mode, u32 uid, u32 gid, u64 size
//   i64 mtime.sec, i32 mtime.nsec
//   i64 ctime.sec, i32 ctime.nsec
//   i64 atime.sec, i32 atime.nsec
//   u8  hash kind (MetadataHashKind), u8 hash length, hash bytes
//
// History event:
//   u8 kind (HistoryEventKind), i64 time.sec, i32 time.nsec,
//   u32 path length, path bytes, metadata record (old), metadata record (new)

// Bytes between the enabled flag and the hash kind byte.
constexpr size_t METADATA_RECORD_FIXED_BYTES = 4 + 4 + 4 + 8 + 3 * (8 + 4);

void encode_metadata_record(std::string &dst, const FileMetadata &metadata);

// Decodes one record starting at *ptr and advances *ptr past it. Returns false
// if the buffer ends before the record does; *ptr is then unspecified.
bool decode_metadata_record(const char **ptr, const char *end, FileMetadata *meta);

// Returns the size of the record starting at ptr, or 0 if [ptr, end) does not
// hold enough bytes to know (callers streaming the record then fetch more).
size_t metadata_record_size(const char *ptr, const char *end);

std::string encode_history_stream(const std::vector<HistoryEvent> &events);

// Decodes as many complete events as the buffer holds. If the stream does not
// start with a run marker, one is synthesised so that every run is delimited.
std::vector<HistoryEvent> decode_history_stream(const std::string &data);

#endif  // !defined(METADATA_CODEC_H)
