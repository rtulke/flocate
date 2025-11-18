#include "metadata_hash.h"

#include <algorithm>
#include <errno.h>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

inline uint64_t rotl64(uint64_t value, int bits)
{
	return (value << bits) | (value >> (64 - bits));
}

inline uint32_t rotr32(uint32_t value, int bits)
{
	return (value >> bits) | (value << (32 - bits));
}

const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint64_t read64(const uint8_t *ptr)
{
	uint64_t val;
	memcpy(&val, ptr, sizeof(val));
	return val;
}

inline uint32_t read32(const uint8_t *ptr)
{
	uint32_t val;
	memcpy(&val, ptr, sizeof(val));
	return val;
}

inline uint64_t xxhash_round(uint64_t acc, uint64_t input)
{
	acc += input * MetadataHashBuilder::XxHash64State::Prime2;
	acc = rotl64(acc, 31);
	acc *= MetadataHashBuilder::XxHash64State::Prime1;
	return acc;
}

inline uint64_t xxhash_merge_round(uint64_t acc, uint64_t val)
{
	val = xxhash_round(0, val);
	acc ^= val;
	acc = acc * MetadataHashBuilder::XxHash64State::Prime1 + MetadataHashBuilder::XxHash64State::Prime4;
	return acc;
}

}  // namespace

void MetadataHashBuilder::XxHash64State::reset(uint64_t seed)
{
	total_len = 0;
	v1 = seed + Prime1 + Prime2;
	v2 = seed + Prime2;
	v3 = seed;
	v4 = seed - Prime1;
	memsize = 0;
	std::fill(std::begin(memory), std::end(memory), 0);
}

void MetadataHashBuilder::XxHash64State::consume(const uint8_t *data, size_t len)
{
	total_len += len;
	const uint8_t *p = data;

	if (memsize + len < 32) {
		memcpy(memory + memsize, data, len);
		memsize += len;
		return;
	}

	if (memsize > 0) {
		size_t fill = 32 - memsize;
		memcpy(memory + memsize, data, fill);
		const uint8_t *ptr = memory;
		v1 = xxhash_round(v1, read64(ptr));
		ptr += 8;
		v2 = xxhash_round(v2, read64(ptr));
		ptr += 8;
		v3 = xxhash_round(v3, read64(ptr));
		ptr += 8;
		v4 = xxhash_round(v4, read64(ptr));
		ptr += 8;
		p += fill;
		len -= fill;
		memsize = 0;
	}

	const uint8_t *end = p + len;
	const uint8_t *limit = end - 32;
	while (p <= limit) {
		v1 = xxhash_round(v1, read64(p));
		p += 8;
		v2 = xxhash_round(v2, read64(p));
		p += 8;
		v3 = xxhash_round(v3, read64(p));
		p += 8;
		v4 = xxhash_round(v4, read64(p));
		p += 8;
	}

	memcpy(memory, p, static_cast<size_t>(end - p));
	memsize = static_cast<size_t>(end - p);
}

uint64_t MetadataHashBuilder::XxHash64State::digest()
{
	uint64_t hash;
	if (total_len >= 32) {
		hash = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
		hash = xxhash_merge_round(hash, v1);
		hash = xxhash_merge_round(hash, v2);
		hash = xxhash_merge_round(hash, v3);
		hash = xxhash_merge_round(hash, v4);
	} else {
		hash = Prime5;
	}

	hash += total_len;
	const uint8_t *p = memory;
	const uint8_t *end = memory + memsize;

	while (p + 8 <= end) {
		hash ^= rotl64(read64(p) * Prime2, 31) * Prime1;
		hash = hash * Prime1 + Prime4;
		p += 8;
	}

	if (p + 4 <= end) {
		hash ^= uint64_t(read32(p)) * Prime1;
		hash = rotl64(hash, 23) * Prime2 + Prime3;
		p += 4;
	}

	while (p < end) {
		hash ^= (*p) * Prime5;
		hash = rotl64(hash, 11) * Prime1;
		++p;
	}

	hash ^= hash >> 33;
	hash *= Prime2;
	hash ^= hash >> 29;
	hash *= Prime3;
	hash ^= hash >> 32;
	return hash;
}

void MetadataHashBuilder::Sha256State::reset()
{
	static const uint32_t initial_h[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	memcpy(h, initial_h, sizeof(h));
	total_len = 0;
	buffer_len = 0;
}

void MetadataHashBuilder::Sha256State::process_block(const uint8_t block[64])
{
	uint32_t w[64];
	for (int i = 0; i < 16; ++i) {
		w[i] = (uint32_t(block[i * 4]) << 24) |
		       (uint32_t(block[i * 4 + 1]) << 16) |
		       (uint32_t(block[i * 4 + 2]) << 8) |
		       uint32_t(block[i * 4 + 3]);
	}
	for (int i = 16; i < 64; ++i) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = h[0];
	uint32_t b = h[1];
	uint32_t c = h[2];
	uint32_t d = h[3];
	uint32_t e = h[4];
	uint32_t f = h[5];
	uint32_t g = h[6];
	uint32_t hval = h[7];

	for (int i = 0; i < 64; ++i) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = hval + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		hval = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hval;
}

void MetadataHashBuilder::Sha256State::consume(const uint8_t *data, size_t len)
{
	total_len += len;
	const uint8_t *p = data;
	if (buffer_len > 0) {
		size_t fill = 64 - buffer_len;
		if (fill > len)
			fill = len;
		memcpy(buffer + buffer_len, data, fill);
		buffer_len += fill;
		p += fill;
		len -= fill;
		if (buffer_len == 64) {
			process_block(buffer);
			buffer_len = 0;
		}
	}
	while (len >= 64) {
		process_block(p);
		p += 64;
		len -= 64;
	}
	if (len > 0) {
		memcpy(buffer, p, len);
		buffer_len = len;
	}
}

void MetadataHashBuilder::Sha256State::finalize(uint8_t out[32])
{
	uint64_t bit_len = total_len * 8;
	buffer[buffer_len++] = 0x80;
	if (buffer_len > 56) {
		while (buffer_len < 64)
			buffer[buffer_len++] = 0;
		process_block(buffer);
		buffer_len = 0;
	}
	while (buffer_len < 56)
		buffer[buffer_len++] = 0;
	for (int i = 7; i >= 0; --i) {
		buffer[buffer_len++] = uint8_t((bit_len >> (i * 8)) & 0xff);
	}
	process_block(buffer);
	for (int i = 0; i < 8; ++i) {
		out[i * 4 + 0] = uint8_t(h[i] >> 24);
		out[i * 4 + 1] = uint8_t(h[i] >> 16);
		out[i * 4 + 2] = uint8_t(h[i] >> 8);
		out[i * 4 + 3] = uint8_t(h[i]);
	}
}

MetadataHashBuilder::MetadataHashBuilder(MetadataHashKind kind)
	: kind(kind)
{
	if (kind == MetadataHashKind::XxHash64) {
		xxh_state.reset(/*seed=*/0);
	} else if (kind == MetadataHashKind::Sha256) {
		sha_state.reset();
	}
}

void MetadataHashBuilder::update(const void *data, size_t len)
{
	if (finished || kind == MetadataHashKind::None || len == 0 || data == nullptr) {
		return;
	}
	const uint8_t *ptr = static_cast<const uint8_t *>(data);
	if (kind == MetadataHashKind::XxHash64) {
		update_xxh64(ptr, len);
	} else if (kind == MetadataHashKind::Sha256) {
		update_sha256(ptr, len);
	}
}

MetadataHash MetadataHashBuilder::finalize()
{
	finished = true;
	if (kind == MetadataHashKind::XxHash64) {
		return finalize_xxh64();
	} else if (kind == MetadataHashKind::Sha256) {
		return finalize_sha256();
	}
	return MetadataHash{};
}

void MetadataHashBuilder::update_xxh64(const uint8_t *data, size_t len)
{
	xxh_state.consume(data, len);
}

MetadataHash MetadataHashBuilder::finalize_xxh64()
{
	MetadataHash hash;
	hash.kind = MetadataHashKind::XxHash64;
	uint64_t value = xxh_state.digest();
	memcpy(hash.value.data(), &value, sizeof(value));
	hash.length = sizeof(value);
	return hash;
}

void MetadataHashBuilder::update_sha256(const uint8_t *data, size_t len)
{
	sha_state.consume(data, len);
}

MetadataHash MetadataHashBuilder::finalize_sha256()
{
	MetadataHash hash;
	hash.kind = MetadataHashKind::Sha256;
	sha_state.finalize(hash.value.data());
	hash.length = 32;
	return hash;
}

bool compute_file_hash_at(int dirfd, const std::string &name, MetadataHashKind kind, MetadataHash *out)
{
	if (out == nullptr) {
		return false;
	}
	if (kind == MetadataHashKind::None) {
		*out = MetadataHash{};
		return true;
	}
	MetadataHashBuilder builder(kind);
	int fd = openat(dirfd, name.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		return false;
	}
	uint8_t buf[1 << 15];
	while (true) {
		ssize_t ret = read(fd, buf, sizeof(buf));
		if (ret == 0) {
			break;
		} else if (ret < 0) {
			int saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return false;
		}
		builder.update(buf, static_cast<size_t>(ret));
	}
	if (close(fd) != 0) {
		return false;
	}
	*out = builder.finalize();
	return true;
}
