#include "metadata_codec.h"

#include <algorithm>
#include <string.h>

using namespace std;

namespace {

void put_u8(string &dst, uint8_t v)
{
	dst.push_back(static_cast<char>(v));
}

void put_u32(string &dst, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		dst.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
	}
}

void put_u64(string &dst, uint64_t v)
{
	for (int i = 0; i < 8; ++i) {
		dst.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
	}
}

void put_time(string &dst, const dir_time &t)
{
	put_u64(dst, static_cast<uint64_t>(t.sec));
	put_u32(dst, static_cast<uint32_t>(t.nsec));
}

uint8_t get_u8(const char *&p)
{
	return static_cast<uint8_t>(*p++);
}

uint32_t get_u32(const char *&p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i) {
		v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
	}
	p += 4;
	return v;
}

uint64_t get_u64(const char *&p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) {
		v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
	}
	p += 8;
	return v;
}

dir_time get_time(const char *&p)
{
	dir_time t;
	t.sec = static_cast<int64_t>(get_u64(p));
	t.nsec = static_cast<int32_t>(get_u32(p));
	return t;
}

}  // namespace

void encode_metadata_record(string &dst, const FileMetadata &metadata)
{
	if (!metadata.enabled) {
		put_u8(dst, 0);
		return;
	}
	put_u8(dst, 1);
	put_u32(dst, metadata.mode);
	put_u32(dst, metadata.uid);
	put_u32(dst, metadata.gid);
	put_u64(dst, metadata.size);
	put_time(dst, metadata.mtime);
	put_time(dst, metadata.ctime);
	put_time(dst, metadata.atime);

	const size_t hash_len = min(metadata.hash.length, metadata.hash.value.size());
	put_u8(dst, static_cast<uint8_t>(metadata.hash.kind));
	put_u8(dst, static_cast<uint8_t>(hash_len));
	dst.append(reinterpret_cast<const char *>(metadata.hash.value.data()), hash_len);
}

size_t metadata_record_size(const char *ptr, const char *end)
{
	if (ptr >= end) {
		return 0;
	}
	if (static_cast<uint8_t>(*ptr) == 0) {
		return 1;
	}
	const size_t up_to_hash_len = 1 + METADATA_RECORD_FIXED_BYTES + 2;
	if (static_cast<size_t>(end - ptr) < up_to_hash_len) {
		return 0;
	}
	const uint8_t hash_len = static_cast<uint8_t>(ptr[up_to_hash_len - 1]);
	const size_t total = up_to_hash_len + hash_len;
	return static_cast<size_t>(end - ptr) >= total ? total : 0;
}

bool decode_metadata_record(const char **ptr, const char *end, FileMetadata *meta)
{
	const size_t size = metadata_record_size(*ptr, end);
	if (size == 0) {
		return false;
	}
	const char *p = *ptr;
	*meta = FileMetadata{};
	if (get_u8(p) == 0) {
		*ptr = p;
		return true;
	}
	meta->enabled = true;
	meta->mode = get_u32(p);
	meta->uid = get_u32(p);
	meta->gid = get_u32(p);
	meta->size = get_u64(p);
	meta->mtime = get_time(p);
	meta->ctime = get_time(p);
	meta->atime = get_time(p);
	meta->hash.kind = static_cast<MetadataHashKind>(get_u8(p));
	const uint8_t hash_len = get_u8(p);
	meta->hash.length = min<size_t>(hash_len, meta->hash.value.size());
	memcpy(meta->hash.value.data(), p, meta->hash.length);
	*ptr += size;
	return true;
}

string encode_history_stream(const vector<HistoryEvent> &events)
{
	string buf;
	for (const HistoryEvent &event : events) {
		put_u8(buf, static_cast<uint8_t>(event.kind));
		put_time(buf, event.event_time);
		put_u32(buf, static_cast<uint32_t>(event.path.size()));
		buf.append(event.path.data(), event.path.size());
		encode_metadata_record(buf, event.old_metadata);
		encode_metadata_record(buf, event.new_metadata);
	}
	return buf;
}

vector<HistoryEvent> decode_history_stream(const string &data)
{
	vector<HistoryEvent> events;
	const char *ptr = data.data();
	const char *end = ptr + data.size();
	const size_t event_header_bytes = 1 + 8 + 4 + 4;
	while (static_cast<size_t>(end - ptr) >= event_header_bytes) {
		HistoryEvent event;
		event.kind = static_cast<HistoryEventKind>(get_u8(ptr));
		event.event_time = get_time(ptr);
		const uint32_t path_len = get_u32(ptr);
		if (static_cast<size_t>(end - ptr) < path_len) {
			break;
		}
		event.path.assign(ptr, path_len);
		ptr += path_len;
		if (!decode_metadata_record(&ptr, end, &event.old_metadata) ||
		    !decode_metadata_record(&ptr, end, &event.new_metadata)) {
			break;
		}
		events.push_back(std::move(event));
	}
	if (!events.empty() && events.front().kind != HistoryEventKind::RunMarker) {
		HistoryEvent marker;
		marker.kind = HistoryEventKind::RunMarker;
		marker.event_time = events.front().event_time;
		events.insert(events.begin(), marker);
	}
	return events;
}
