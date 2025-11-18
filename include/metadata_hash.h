#ifndef METADATA_HASH_H
#define METADATA_HASH_H 1

#include "metadata.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

class MetadataHashBuilder {
public:
	explicit MetadataHashBuilder(MetadataHashKind kind);
	void update(const void *data, size_t len);
	MetadataHash finalize();
	MetadataHashKind get_kind() const { return kind; }

	void update_xxh64(const uint8_t *data, size_t len);
	MetadataHash finalize_xxh64();
	void update_sha256(const uint8_t *data, size_t len);
	MetadataHash finalize_sha256();

private:
	MetadataHashKind kind;
	bool finished = false;

	struct XxHash64State {
		static constexpr uint64_t Prime1 = 11400714785074694791ULL;
		static constexpr uint64_t Prime2 = 14029467366897019727ULL;
		static constexpr uint64_t Prime3 =  1609587929392839161ULL;
		static constexpr uint64_t Prime4 =  9650029242287828579ULL;
		static constexpr uint64_t Prime5 =  2870177450012600261ULL;

		uint64_t total_len = 0;
		uint64_t v1 = Prime1 + Prime2;
		uint64_t v2 = Prime2;
		uint64_t v3 = 0;
		uint64_t v4 = -Prime1;
		size_t memsize = 0;
		uint8_t memory[32];

		void reset(uint64_t seed);
		void consume(const uint8_t *data, size_t len);
		uint64_t digest();
	} xxh_state;

	struct Sha256State {
		uint32_t h[8];
		uint64_t total_len = 0;
		size_t buffer_len = 0;
		uint8_t buffer[64];

		void reset();
		void consume(const uint8_t *data, size_t len);
		void finalize(uint8_t out[32]);

	private:
		void process_block(const uint8_t block[64]);
	} sha_state;
};

bool compute_file_hash_at(int dirfd, const std::string &name, MetadataHashKind kind, MetadataHash *out);

#endif  // METADATA_HASH_H
