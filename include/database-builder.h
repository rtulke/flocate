#ifndef _DATABASE_BUILDER_H
#define _DATABASE_BUILDER_H 1

#include "db.h"
#include "metadata.h"

#include <chrono>
#include <fcntl.h>
#include <memory>
#include <random>
#include <stddef.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zstd.h>

class PostingListBuilder;

class DatabaseReceiver {
public:
	virtual ~DatabaseReceiver() = default;
	virtual void add_file(const FileRecord &record) = 0;
	virtual void flush_block() = 0;
	virtual void finish() { flush_block(); }

	// EncodingCorpus only.
	virtual size_t num_files_seen() const { return -1; }
};

class DictionaryBuilder : public DatabaseReceiver {
public:
	DictionaryBuilder(size_t blocks_to_keep, size_t block_size)
		: blocks_to_keep(blocks_to_keep), block_size(block_size) {}
	void add_file(const FileRecord &record) override;
	void flush_block() override;
	std::string train(size_t buf_size);

private:
	const size_t blocks_to_keep, block_size;
	std::string current_block;
	uint64_t block_num = 0;
	size_t num_files_in_block = 0;

	std::mt19937 reservoir_rand{ 1234 };  // Fixed seed for reproducibility.
	bool keep_current_block = true;
	int64_t slot_for_current_block = -1;

	std::vector<std::string> sampled_blocks;
	std::vector<size_t> lengths;
};

class EncodingCorpus;

class DatabaseBuilder {
public:
	DatabaseBuilder(const char *outfile, gid_t owner, int block_size, std::string dictionary, bool check_visibility);
	DatabaseReceiver *start_corpus(bool store_dir_times, bool store_metadata);
	void set_next_dictionary(std::string next_dictionary);
	void set_conf_block(std::string conf_block);
	void set_history_events(std::vector<HistoryEvent> events);
	void finish_corpus();

private:
	FILE *outfp;
	std::string outfile;
	std::string temp_filename;
	Header hdr;
	const int block_size;
	std::chrono::steady_clock::time_point corpus_start;
	EncodingCorpus *corpus = nullptr;
	ZSTD_CDict *cdict = nullptr;
	std::string next_dictionary, conf_block;
	std::vector<HistoryEvent> history_events;
};

#endif  // !defined(_DATABASE_BUILDER_H)
