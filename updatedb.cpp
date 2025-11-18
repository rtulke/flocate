/* updatedb(8).

Copyright (C) 2005, 2007, 2008 Red Hat, Inc. All rights reserved.

This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmac <mitr@redhat.com>


plocate modifications: Copyright (C) 2020 Steinar H. Gunderson.
plocate parts and modifications are licensed under the GPLv2 or, at your option,
any later version.
 */

#include "bind-mount.h"
#include "complete_pread.h"
#include "conf.h"
#include "database-builder.h"
#include "db.h"
#include "dprintf.h"
#include "io_uring_engine.h"
#include "metadata_hash.h"
#include "lib.h"

#include <algorithm>
#include <arpa/inet.h>
#include <assert.h>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <iosfwd>
#include <math.h>
#include <memory>
#include <mntent.h>
#include <random>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std;
using namespace std::chrono;

/* Next conf_prunepaths entry */
static size_t conf_prunepaths_index; /* = 0; */

void usage()
{
	printf(
		"Usage: updatedb PLOCATE_DB\n"
		"\n"
		"Generate plocate index from mlocate.db, typically /var/lib/mlocate/mlocate.db.\n"
		"Normally, the destination should be /var/lib/mlocate/plocate.db.\n"
		"\n"
		"  -b, --block-size SIZE  number of filenames to store in each block (default 32)\n"
		"  -p, --plaintext        input is a plaintext file, not an mlocate database\n"
		"      --help             print this help\n"
		"      --version          print version information\n");
}

void version()
{
	printf("updatedb %s\n", PACKAGE_VERSION);
	printf("Copyright (C) 2007 Red Hat, Inc. All rights reserved.\n");
	printf("Copyright 2020 Steinar H. Gunderson\n");
	printf("This software is distributed under the GPL v.2.\n");
	printf("\n");
	printf("This program is provided with NO WARRANTY, to the extent permitted by law.\n");
}

int opendir_noatime(int dirfd, const char *path)
{
	static bool noatime_failed = false;

	if (!noatime_failed) {
#ifdef O_NOATIME
		int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY | O_NOATIME);
#else
		int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY);
#endif
		if (fd != -1) {
			return fd;
		} else if (errno == EPERM) {
			/* EPERM is fairly O_NOATIME-specific; missing access rights cause
			   EACCES. */
			noatime_failed = true;
			// Retry below.
		} else {
			return -1;
		}
	}
	return openat(dirfd, path, O_RDONLY | O_DIRECTORY);
}

bool time_is_current(const dir_time &t)
{
	static dir_time cache{ 0, 0 };

	/* This is more difficult than it should be because Linux uses a cheaper time
	   source for filesystem timestamps than for gettimeofday() and they can get
	   slightly out of sync, see
	   https://bugzilla.redhat.com/show_bug.cgi?id=244697 .  This affects even
	   nanosecond timestamps (and don't forget that tv_nsec existence doesn't
	   guarantee that the underlying filesystem has such resolution - it might be
	   microseconds or even coarser).

	   The worst case is probably FAT timestamps with 2-second resolution
	   (although using such a filesystem violates POSIX file times requirements).

	   So, to be on the safe side, require a >3.0 second difference (2 seconds to
	   make sure the FAT timestamp changed, 1 more to account for the Linux
	   timestamp races).  This large margin might make updatedb marginally more
	   expensive, but it only makes a difference if the directory was very
	   recently updated _and_ is will not be updated again until the next
	   updatedb run; this is not likely to happen for most directories. */

	/* Cache gettimeofday () results to rule out obviously old time stamps;
	   CACHE contains the earliest time we reject as too current. */
	if (t < cache) {
		return false;
	}

	struct timeval tv;
	gettimeofday(&tv, nullptr);
	cache.sec = tv.tv_sec - 3;
	cache.nsec = tv.tv_usec * 1000;

	return t >= cache;
}

struct entry {
	string name;
	bool is_directory;

	// For directories only:
	int fd = -1;
	dir_time dt = unknown_dir_time;
	dir_time db_modified = unknown_dir_time;
	dev_t dev;
	FileMetadata metadata;
	FileMetadata previous_metadata;
	bool existed_before = false;
	bool matched_in_new_scan = false;
};

bool filesystem_is_excluded(const string &path)
{
	if (conf_debug_pruning) {
		fprintf(stderr, "Checking whether filesystem `%s' is excluded:\n", path.c_str());
	}
	FILE *f = setmntent("/proc/mounts", "r");
	if (f == nullptr) {
		return false;
	}

	struct mntent *me;
	while ((me = getmntent(f)) != nullptr) {
		if (conf_debug_pruning) {
			fprintf(stderr, " `%s', type `%s'\n", me->mnt_dir, me->mnt_type);
		}
		if (path != me->mnt_dir) {
			continue;
		}
		string type(me->mnt_type);
		for (char &p : type) {
			p = toupper(p);
		}
		bool exclude = (find(conf_prunefs.begin(), conf_prunefs.end(), type) != conf_prunefs.end());
		if (exclude && conf_debug_pruning) {
			fprintf(stderr, " => excluded due to filesystem type\n");
		}
		endmntent(f);
		return exclude;
	}
	if (conf_debug_pruning) {
		fprintf(stderr, "...not found in mount list\n");
	}
	endmntent(f);
	return false;
}

dir_time get_dirtime_from_stat(const struct stat &buf)
{
	dir_time ctime{ buf.st_ctim.tv_sec, int32_t(buf.st_ctim.tv_nsec) };
	dir_time mtime{ buf.st_mtim.tv_sec, int32_t(buf.st_mtim.tv_nsec) };
	dir_time dt = max(ctime, mtime);

	if (time_is_current(dt)) {
		/* The directory might be changing right now and we can't be sure the
		   timestamp will be changed again if more changes happen very soon, mark
		   the timestamp as invalid to force rescanning the directory next time
		   updatedb is run. */
		return unknown_dir_time;
	} else {
		return dt;
	}
}

FileMetadata metadata_from_stat(const struct stat &buf)
{
	FileMetadata metadata;
	metadata.enabled = true;
	metadata.mode = buf.st_mode;
	metadata.uid = buf.st_uid;
	metadata.gid = buf.st_gid;
	metadata.size = buf.st_size;
	metadata.mtime = dir_time{ buf.st_mtim.tv_sec, int32_t(buf.st_mtim.tv_nsec) };
	metadata.ctime = dir_time{ buf.st_ctim.tv_sec, int32_t(buf.st_ctim.tv_nsec) };
	metadata.atime = dir_time{ buf.st_atim.tv_sec, int32_t(buf.st_atim.tv_nsec) };
	return metadata;
}

dir_time current_realtime()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return dir_time{ ts.tv_sec, int32_t(ts.tv_nsec) };
}

void record_history_event(vector<HistoryEvent> *history, HistoryEventKind kind, const string &path, const FileMetadata &old_meta, const FileMetadata &new_meta)
{
	if (history == nullptr) {
		return;
	}
	HistoryEvent event;
	event.kind = kind;
	event.path = path;
	event.event_time = current_realtime();
	event.old_metadata = old_meta;
	event.new_metadata = new_meta;
	history->push_back(move(event));
}

// Represents the old database we are updating.
class ExistingDB {
public:
	explicit ExistingDB(int fd);
	~ExistingDB();

	pair<string, dir_time> read_next();
	void unread(pair<string, dir_time> record)
	{
		unread_record = move(record);
		unread_metadata_record = last_metadata_record;
		unread_metadata_valid = true;
	}
	string read_next_dictionary() const;
	bool get_error() const { return error; }
	const FileMetadata &last_metadata() const { return last_metadata_record; }

private:
	bool ensure_metadata_bytes(size_t need);
	bool refill_metadata_block();
	void read_next_metadata_record();

	const int fd;
	Header hdr;

	uint32_t current_docid = 0;

	string current_filename_block;
	const char *current_filename_ptr = nullptr, *current_filename_end = nullptr;

	off_t compressed_dir_time_pos;
	string compressed_dir_time;
	string current_dir_time_block;
	const char *current_dir_time_ptr = nullptr, *current_dir_time_end = nullptr;

	pair<string, dir_time> unread_record;
	FileMetadata unread_metadata_record;
	bool unread_metadata_valid = false;

	bool metadata_available = false;
	off_t compressed_metadata_pos = 0;
	string compressed_metadata;
	string current_metadata_block;
	const char *current_metadata_ptr = nullptr, *current_metadata_end = nullptr;
	FileMetadata last_metadata_record;

	// Used in one-shot mode, repeatedly.
	ZSTD_DCtx *ctx;

	// Used in streaming mode.
	ZSTD_DCtx *dir_time_ctx;
	ZSTD_DCtx *metadata_ctx = nullptr;

	ZSTD_DDict *ddict = nullptr;

	// If true, we've discovered an error or EOF, and will return only
	// empty data from here.
	bool eof = false, error = false;
};

ExistingDB::ExistingDB(int fd)
	: fd(fd)
{
	if (fd == -1) {
		error = true;
		return;
	}

	if (!try_complete_pread(fd, &hdr, sizeof(hdr), /*offset=*/0)) {
		if (conf_verbose) {
			perror("pread(header)");
		}
		error = true;
		return;
	}
	if (memcmp(hdr.magic, "\0plocate", 8) != 0) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had header mismatch, ignoring.\n");
		}
		error = true;
		return;
	}
	if ((hdr.version != 1 && hdr.version != 2) || hdr.max_version < 2) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had version mismatch (version=%d max_version=%d), ignoring.\n",
			        hdr.version, hdr.max_version);
		}
		error = true;
		return;
	}

	// Compare the configuration block with our current one.
	if (hdr.conf_block_length_bytes != conf_block.size()) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had different configuration block (size mismatch), ignoring.\n");
		}
		error = true;
		return;
	}
	string str;
	str.resize(hdr.conf_block_length_bytes);
	if (!try_complete_pread(fd, str.data(), hdr.conf_block_length_bytes, hdr.conf_block_offset_bytes)) {
		if (conf_verbose) {
			perror("pread(conf_block)");
		}
		error = true;
		return;
	}
	if (str != conf_block) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had different configuration block (contents mismatch), ignoring.\n");
		}
		error = true;
		return;
	}

	// Read dictionary, if it exists.
	if (hdr.zstd_dictionary_length_bytes > 0) {
		string dictionary;
		dictionary.resize(hdr.zstd_dictionary_length_bytes);
		if (try_complete_pread(fd, &dictionary[0], hdr.zstd_dictionary_length_bytes, hdr.zstd_dictionary_offset_bytes)) {
			ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
		} else {
			if (conf_verbose) {
				perror("pread(dictionary)");
			}
			error = true;
			return;
		}
	}
	compressed_dir_time_pos = hdr.directory_data_offset_bytes;

	if (hdr.metadata_length_bytes != 0 && (hdr.metadata_flags & METADATA_FLAG_POSIX_V1)) {
		metadata_available = true;
		compressed_metadata_pos = hdr.metadata_offset_bytes;
		metadata_ctx = ZSTD_createDCtx();
		current_metadata_ptr = current_metadata_block.data();
		current_metadata_end = current_metadata_block.data();
	}

	ctx = ZSTD_createDCtx();
	dir_time_ctx = ZSTD_createDCtx();
}

ExistingDB::~ExistingDB()
{
	if (fd != -1) {
		close(fd);
	}
}

bool ExistingDB::ensure_metadata_bytes(size_t need)
{
	if (!metadata_available) {
		return false;
	}
	while (size_t(current_metadata_end - current_metadata_ptr) < need) {
		if (!refill_metadata_block()) {
			return false;
		}
	}
	return true;
}

bool ExistingDB::refill_metadata_block()
{
	if (!metadata_available) {
		return false;
	}

	if (current_metadata_ptr != nullptr) {
		const size_t consumed = current_metadata_ptr - current_metadata_block.data();
		current_metadata_block.erase(current_metadata_block.begin(), current_metadata_block.begin() + consumed);
	}

	size_t existing_data = current_metadata_block.size();
	current_metadata_block.resize(existing_data + 4096);

	ZSTD_outBuffer outbuf;
	outbuf.dst = current_metadata_block.data() + existing_data;
	outbuf.size = 4096;
	outbuf.pos = 0;

	ZSTD_inBuffer inbuf;
	inbuf.src = compressed_metadata.data();
	inbuf.size = compressed_metadata.size();
	inbuf.pos = 0;

	int err = ZSTD_decompressStream(metadata_ctx, &outbuf, &inbuf);
	if (err < 0) {
		if (conf_verbose) {
			fprintf(stderr, "ZSTD_decompressStream(metadata) failed\n");
		}
		metadata_available = false;
		return false;
	}

	compressed_metadata.erase(compressed_metadata.begin(), compressed_metadata.begin() + inbuf.pos);
	current_metadata_block.resize(existing_data + outbuf.pos);

	if (outbuf.pos == 0 && inbuf.pos == 0) {
		if (compressed_metadata_pos >= hdr.metadata_offset_bytes + hdr.metadata_length_bytes) {
			metadata_available = false;
			return false;
		}
		size_t bytes_to_read = min<size_t>(4096, hdr.metadata_offset_bytes + hdr.metadata_length_bytes - compressed_metadata_pos);
		char buf[4096];
		if (!try_complete_pread(fd, buf, bytes_to_read, compressed_metadata_pos)) {
			if (conf_verbose) {
				perror("pread(metadata)");
			}
			metadata_available = false;
			return false;
		}
		compressed_metadata_pos += bytes_to_read;
		compressed_metadata.insert(compressed_metadata.end(), buf, buf + bytes_to_read);
		return refill_metadata_block();
	}

	current_metadata_ptr = current_metadata_block.data();
	current_metadata_end = current_metadata_block.data() + current_metadata_block.size();
	return current_metadata_ptr != current_metadata_end;
}

void ExistingDB::read_next_metadata_record()
{
	if (!metadata_available) {
		last_metadata_record = {};
		return;
	}

	if (!ensure_metadata_bytes(1)) {
		last_metadata_record = {};
		metadata_available = false;
		return;
	}

	FileMetadata meta;
	meta.enabled = (*reinterpret_cast<const unsigned char *>(current_metadata_ptr)) != 0;
	++current_metadata_ptr;

	if (!meta.enabled) {
		last_metadata_record = meta;
		return;
	}

	const size_t fixed_bytes = sizeof(meta.mode) + sizeof(meta.uid) + sizeof(meta.gid) + sizeof(meta.size) +
		sizeof(meta.mtime.sec) + sizeof(meta.mtime.nsec) +
		sizeof(meta.ctime.sec) + sizeof(meta.ctime.nsec) +
		sizeof(meta.atime.sec) + sizeof(meta.atime.nsec);
	if (!ensure_metadata_bytes(fixed_bytes + 2)) {
		last_metadata_record = {};
		metadata_available = false;
		return;
	}

	auto read_scalar = [this](auto *dst) {
		memcpy(dst, current_metadata_ptr, sizeof(*dst));
		current_metadata_ptr += sizeof(*dst);
	};

	read_scalar(&meta.mode);
	read_scalar(&meta.uid);
	read_scalar(&meta.gid);
	read_scalar(&meta.size);
	read_scalar(&meta.mtime.sec);
	read_scalar(&meta.mtime.nsec);
	read_scalar(&meta.ctime.sec);
	read_scalar(&meta.ctime.nsec);
	read_scalar(&meta.atime.sec);
	read_scalar(&meta.atime.nsec);

	meta.hash.kind = static_cast<MetadataHashKind>(static_cast<unsigned char>(*current_metadata_ptr++));
	uint8_t hash_len = static_cast<uint8_t>(*current_metadata_ptr++);
	if (!ensure_metadata_bytes(hash_len)) {
		last_metadata_record = {};
		metadata_available = false;
		return;
	}
	meta.hash.length = min<size_t>(hash_len, meta.hash.value.size());
	if (meta.hash.length != 0) {
		memcpy(meta.hash.value.data(), current_metadata_ptr, meta.hash.length);
	}
	current_metadata_ptr += hash_len;
	last_metadata_record = meta;
}

pair<string, dir_time> ExistingDB::read_next()
{
	if (!unread_record.first.empty()) {
		auto ret = move(unread_record);
		unread_record.first.clear();
		if (unread_metadata_valid) {
			last_metadata_record = unread_metadata_record;
			unread_metadata_valid = false;
		} else {
			last_metadata_record = {};
		}
		return ret;
	}

	if (eof || error) {
		return { "", not_a_dir };
	}

	// See if we need to read a new filename block.
	if (current_filename_ptr == nullptr) {
		if (current_docid >= hdr.num_docids) {
			eof = true;
			return { "", not_a_dir };
		}

		// Read the file offset from this docid and the next one.
		// This is always allowed, since we have a sentinel block at the end.
		off_t offset_for_block = hdr.filename_index_offset_bytes + current_docid * sizeof(uint64_t);
		uint64_t vals[2];
		if (!try_complete_pread(fd, vals, sizeof(vals), offset_for_block)) {
			if (conf_verbose) {
				perror("pread(offset)");
			}
			error = true;
			return { "", not_a_dir };
		}

		off_t offset = vals[0];
		size_t compressed_len = vals[1] - vals[0];
		unique_ptr<char[]> compressed(new char[compressed_len]);
		if (!try_complete_pread(fd, compressed.get(), compressed_len, offset)) {
			if (conf_verbose) {
				perror("pread(block)");
			}
			error = true;
			return { "", not_a_dir };
		}

		unsigned long long uncompressed_len = ZSTD_getFrameContentSize(compressed.get(), compressed_len);
		if (uncompressed_len == ZSTD_CONTENTSIZE_UNKNOWN || uncompressed_len == ZSTD_CONTENTSIZE_ERROR) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_getFrameContentSize() failed\n");
			}
			error = true;
			return { "", not_a_dir };
		}

		string block;
		block.resize(uncompressed_len + 1);

		size_t err;
		if (ddict != nullptr) {
			err = ZSTD_decompress_usingDDict(ctx, &block[0], block.size(), compressed.get(),
			                                 compressed_len, ddict);
		} else {
			err = ZSTD_decompressDCtx(ctx, &block[0], block.size(), compressed.get(),
			                          compressed_len);
		}
		if (ZSTD_isError(err)) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_decompress(): %s\n", ZSTD_getErrorName(err));
			}
			error = true;
			return { "", not_a_dir };
		}
		block[block.size() - 1] = '\0';
		current_filename_block = move(block);
		current_filename_ptr = current_filename_block.data();
		current_filename_end = current_filename_block.data() + current_filename_block.size();
		++current_docid;
	}

	// See if we need to read more directory time data.
	while (current_dir_time_ptr == current_dir_time_end ||
	       (*current_dir_time_ptr != 0 &&
	        size_t(current_dir_time_end - current_dir_time_ptr) < sizeof(dir_time) + 1)) {
		if (current_dir_time_ptr != nullptr) {
			const size_t bytes_consumed = current_dir_time_ptr - current_dir_time_block.data();
			current_dir_time_block.erase(current_dir_time_block.begin(), current_dir_time_block.begin() + bytes_consumed);
		}

		// See if we can get more data out without reading more.
		const size_t existing_data = current_dir_time_block.size();
		current_dir_time_block.resize(existing_data + 4096);

		ZSTD_outBuffer outbuf;
		outbuf.dst = current_dir_time_block.data() + existing_data;
		outbuf.size = 4096;
		outbuf.pos = 0;

		ZSTD_inBuffer inbuf;
		inbuf.src = compressed_dir_time.data();
		inbuf.size = compressed_dir_time.size();
		inbuf.pos = 0;

		int err = ZSTD_decompressStream(dir_time_ctx, &outbuf, &inbuf);
		if (err < 0) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_decompress(): %s\n", ZSTD_getErrorName(err));
			}
			error = true;
			return { "", not_a_dir };
		}
		compressed_dir_time.erase(compressed_dir_time.begin(), compressed_dir_time.begin() + inbuf.pos);
		current_dir_time_block.resize(existing_data + outbuf.pos);

		if (inbuf.pos == 0 && outbuf.pos == 0) {
			// No movement, we'll need to try to read more data.
			char buf[4096];
			size_t bytes_to_read = min<size_t>(
				hdr.directory_data_offset_bytes + hdr.directory_data_length_bytes - compressed_dir_time_pos,
				sizeof(buf));
			if (bytes_to_read == 0) {
				error = true;
				return { "", not_a_dir };
			}
			if (!try_complete_pread(fd, buf, bytes_to_read, compressed_dir_time_pos)) {
				if (conf_verbose) {
					perror("pread(dirtime)");
				}
				error = true;
				return { "", not_a_dir };
			}
			compressed_dir_time_pos += bytes_to_read;
			compressed_dir_time.insert(compressed_dir_time.end(), buf, buf + bytes_to_read);

			// Next iteration will now try decompressing more.
		}

		current_dir_time_ptr = current_dir_time_block.data();
		current_dir_time_end = current_dir_time_block.data() + current_dir_time_block.size();
	}

	string filename = current_filename_ptr;
	current_filename_ptr += filename.size() + 1;
	if (current_filename_ptr == current_filename_end) {
		// End of this block.
		current_filename_ptr = nullptr;
	}

	if (*current_dir_time_ptr == 0) {
		++current_dir_time_ptr;
		if (!filename.empty()) {
			read_next_metadata_record();
		}
		return { move(filename), not_a_dir };
	} else {
		++current_dir_time_ptr;
		dir_time dt;
		memcpy(&dt.sec, current_dir_time_ptr, sizeof(dt.sec));
		current_dir_time_ptr += sizeof(dt.sec);
		memcpy(&dt.nsec, current_dir_time_ptr, sizeof(dt.nsec));
		current_dir_time_ptr += sizeof(dt.nsec);
		if (!filename.empty()) {
			read_next_metadata_record();
		}
		return { move(filename), dt };
	}
}

string ExistingDB::read_next_dictionary() const
{
	if (hdr.next_zstd_dictionary_length_bytes == 0 || hdr.next_zstd_dictionary_length_bytes > 1048576) {
		return "";
	}
	string str;
	str.resize(hdr.next_zstd_dictionary_length_bytes);
	if (!try_complete_pread(fd, str.data(), hdr.next_zstd_dictionary_length_bytes, hdr.next_zstd_dictionary_offset_bytes)) {
		if (conf_verbose) {
			perror("pread(next_dictionary)");
		}
		return "";
	}
	return str;
}

// Scans the directory with absolute path “path”, which is opened as “fd”.
// Uses relative paths and openat() only, evading any issues with PATH_MAX
// and time-of-check-time-of-use race conditions. (mlocate's updatedb
// does a much more complicated dance with changing the current working
// directory, probably in the interest of portability to old platforms.)
// “parent_dev” must be the device of the parent directory of “path”.
//
// Takes ownership of fd.
int scan(const string &path, int fd, dev_t parent_dev, dir_time modified, dir_time db_modified, ExistingDB *existing_db, DatabaseReceiver *corpus, DictionaryBuilder *dict_builder, vector<HistoryEvent> *history_events)
{
	if (string_list_contains_dir_path(&conf_prunepaths, &conf_prunepaths_index, path)) {
		if (conf_debug_pruning) {
			fprintf(stderr, "Skipping `%s': in prunepaths\n", path.c_str());
		}
		close(fd);
		return 0;
	}
	if (conf_prune_bind_mounts && is_bind_mount(path.c_str())) {
		if (conf_debug_pruning) {
			fprintf(stderr, "Skipping `%s': bind mount\n", path.c_str());
		}
		close(fd);
		return 0;
	}

	// We read in the old directory no matter whether it is current or not,
	// because even if we're not going to use it, we'll need the modification directory
	// of any subdirectories.

	// Skip over anything before this directory; it is stuff that we would have
	// consumed earlier if we wanted it.
	for (;;) {
		pair<string, dir_time> record = existing_db->read_next();
		if (record.first.empty()) {
			break;
		}
		if (dir_path_cmp(path, record.first) <= 0) {
			existing_db->unread(move(record));
			break;
		}
	}

	// Now read everything in this directory.
	vector<entry> db_entries;
	const string path_plus_slash = path.back() == '/' ? path : path + '/';
	for (;;) {
		pair<string, dir_time> record = existing_db->read_next();
		if (record.first.empty()) {
			break;
		}

		if (record.first.rfind(path_plus_slash, 0) != 0) {
			// No longer starts with path, so we're in a different directory.
			existing_db->unread(move(record));
			break;
		}
		if (record.first.find_first_of('/', path_plus_slash.size()) != string::npos) {
			// Entered into a subdirectory of a subdirectory.
			// Due to our ordering, this also means we're done.
			existing_db->unread(move(record));
			break;
		}

		entry e;
		e.name = record.first.substr(path_plus_slash.size());
		e.is_directory = (record.second.sec >= 0);
		e.db_modified = record.second;
		e.previous_metadata = existing_db->last_metadata();
		e.existed_before = true;
		e.matched_in_new_scan = false;
		db_entries.push_back(e);
	}

	DIR *dir = nullptr;
	vector<entry> entries;
	if (!existing_db->get_error() && db_modified.sec > 0 &&
	    modified.sec == db_modified.sec && modified.nsec == db_modified.nsec) {
		// Not changed since the last database, so we can replace the readdir()
		// by reading from the database. (We still need to open and stat everything,
		// though, but that happens in a later step.)
		entries = move(db_entries);
		if (conf_verbose) {
			for (const entry &e : entries) {
				printf("%s/%s\n", path.c_str(), e.name.c_str());
			}
		}
	} else {
		dir = fdopendir(fd);  // Takes over ownership of fd.
		if (dir == nullptr) {
			// fdopendir() wants to fstat() the fd to verify that it's indeed
			// a directory, which can seemingly fail on at least CIFS filesystems
			// if the server feels like it. We treat this as if we had an error
			// on opening it, ie., ignore the directory.
			close(fd);
			return 0;
		}

		dirent *de;
		while ((de = readdir(dir)) != nullptr) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			if (strlen(de->d_name) == 0) {
				/* Unfortunately, this does happen, and mere assert() does not give
				   users enough information to complain to the right people. */
				fprintf(stderr, "file system error: zero-length file name in directory %s", path.c_str());
				continue;
			}

			entry e;
			e.name = de->d_name;
			if (de->d_type == DT_UNKNOWN) {
				// Evidently some file systems, like older versions of XFS
				// (mkfs.xfs -m crc=0 -n ftype=0), can return this,
				// and we need a stat(). If we wanted to optimize for this,
				// we could probably defer it to later (we're stat-ing directories
				// when recursing), but this is rare, and not really worth it --
				// the second stat() will be cached anyway.
				struct stat buf;
				if (fstatat(fd, de->d_name, &buf, AT_SYMLINK_NOFOLLOW) == 0 &&
				    S_ISDIR(buf.st_mode)) {
					e.is_directory = true;
				} else {
					e.is_directory = false;
				}
			} else {
				e.is_directory = (de->d_type == DT_DIR);
			}

			if (conf_verbose) {
				printf("%s/%s\n", path.c_str(), de->d_name);
			}
			entries.push_back(move(e));
		}

		sort(entries.begin(), entries.end(), [](const entry &a, const entry &b) {
			return a.name < b.name;
		});

	// Load directory modification times from the old database.
	auto db_it = db_entries.begin();
	for (entry &e : entries) {
		for (; db_it != db_entries.end(); ++db_it) {
			if (e.name < db_it->name) {
				break;
			}
			if (e.name == db_it->name) {
				e.db_modified = db_it->db_modified;
				e.previous_metadata = db_it->previous_metadata;
				e.existed_before = true;
				db_it->matched_in_new_scan = true;
				break;
			}
		}
	}

	const int parent_fd = (dir != nullptr) ? dirfd(dir) : fd;
	for (entry &e : entries) {
		struct stat sb;
		if (fstatat(parent_fd, e.name.c_str(), &sb, AT_SYMLINK_NOFOLLOW) != 0) {
			continue;
		}
		e.metadata = metadata_from_stat(sb);
		if (S_ISDIR(sb.st_mode)) {
			e.dt = get_dirtime_from_stat(sb);
		} else {
			e.dt = not_a_dir;
		}

		if (conf_metadata_hash_kind != MetadataHashKind::None && S_ISREG(sb.st_mode)) {
			if (!compute_file_hash_at(parent_fd, e.name, conf_metadata_hash_kind, &e.metadata.hash)) {
				if (conf_verbose) {
					perror((path_plus_slash + e.name).c_str());
				}
				e.metadata.hash = MetadataHash{};
			}
		}

		const string full_path = path_plus_slash + e.name;
		if (!e.existed_before) {
			record_history_event(history_events, HistoryEventKind::Added, full_path, FileMetadata{}, e.metadata);
		} else if (!metadata_equals(e.metadata, e.previous_metadata)) {
			record_history_event(history_events, HistoryEventKind::Modified, full_path, e.previous_metadata, e.metadata);
		}
	}

	// For each entry, we want to add it to the database. but this includes the modification time
	// for directories, which means we need to open and stat it at this point.
	//
	// This means we may need to have many directories open at the same time, but it seems to be
	// the simplest (only?) way of being compatible with mlocate's notion of listing all contents
	// of a given directory before recursing, without buffering even more information. Hopefully,
	// we won't go out of file descriptors here (it could happen if someone has tens of thousands
	// of subdirectories in a single directory); if so, the admin will need to raise the limit.
	for (entry &e : entries) {
		if (!e.is_directory) {
			e.dt = not_a_dir;
			continue;
		}

		if (find(conf_prunenames.begin(), conf_prunenames.end(), e.name) != conf_prunenames.end()) {
			if (conf_debug_pruning) {
				fprintf(stderr, "Skipping `%s': in prunenames\n", e.name.c_str());
			}
			continue;
		}

		e.fd = opendir_noatime(parent_fd, e.name.c_str());
		if (e.fd == -1) {
			if (errno == EMFILE || errno == ENFILE) {
				// The admin probably wants to know about this.
				perror((path_plus_slash + e.name).c_str());

				rlimit rlim;
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1) {
					fprintf(stderr, "Hint: Try `ulimit -n 131072' or similar.\n");
				} else {
					fprintf(stderr, "Hint: Try `ulimit -n %" PRIu64 " or similar (current limit is %" PRIu64 ").\n",
					        static_cast<uint64_t>(rlim.rlim_cur * 2), static_cast<uint64_t>(rlim.rlim_cur));
				}
				exit(1);
			}
			continue;
		}

		struct stat buf;
		if (fstat(e.fd, &buf) != 0) {
			// It's possible that this is a filesystem that's excluded
			// (and the failure is e.g. because the network is down).
			// As a last-ditch effort, we try to check that before dying,
			// i.e., duplicate the check from further down.
			//
			// It would be better to be able to run filesystem_is_excluded()
			// for cheap on everything and just avoid the stat, but it seems
			// hard to do that without any kind of raciness.
			if (filesystem_is_excluded(path_plus_slash + e.name)) {
				close(e.fd);
				e.fd = -1;
				continue;
			}

			perror((path_plus_slash + e.name).c_str());
			exit(1);
		}

		e.metadata = metadata_from_stat(buf);
		e.dev = buf.st_dev;
		if (buf.st_dev != parent_dev) {
			if (filesystem_is_excluded(path_plus_slash + e.name)) {
				close(e.fd);
				e.fd = -1;
				continue;
			}
		}

		e.dt = get_dirtime_from_stat(buf);
	}

	for (const entry &old_entry : db_entries) {
		if (!old_entry.matched_in_new_scan) {
			record_history_event(history_events, HistoryEventKind::Removed, path_plus_slash + old_entry.name, old_entry.previous_metadata, FileMetadata{});
		}
	}

	// Actually add all the entries we figured out dates for above.
	for (const entry &e : entries) {
		FileRecord record;
		record.path = path_plus_slash + e.name;
		record.dir_timestamp = e.dt;
		record.metadata = e.metadata;
		corpus->add_file(record);
		dict_builder->add_file(record);
	}

	// Now scan subdirectories.
	for (const entry &e : entries) {
			if (e.is_directory && e.fd != -1) {
				int ret = scan(path_plus_slash + e.name, e.fd, e.dev, e.dt, e.db_modified, existing_db, corpus, dict_builder, history_events);
			if (ret == -1) {
				// TODO: The unscanned file descriptors will leak, but it doesn't really matter,
				// as we're about to exit.
				closedir(dir);
				return -1;
			}
		}
	}

	if (dir == nullptr) {
		close(fd);
	} else {
		closedir(dir);
	}
	return 0;
}

int main(int argc, char **argv)
{
	// We want to bump the file limit; do it if we can (usually we are root
	// and can set whatever we want). 128k should be ample for most setups.
	rlimit rlim;
	if (getrlimit(RLIMIT_NOFILE, &rlim) != -1) {
		// Even root cannot increase rlim_cur beyond rlim_max,
		// so we need to try to increase rlim_max first.
		// Ignore errors, though.
		if (rlim.rlim_max < 131072) {
			rlim.rlim_max = 131072;
			setrlimit(RLIMIT_NOFILE, &rlim);
			getrlimit(RLIMIT_NOFILE, &rlim);
		}

		rlim_t wanted = std::max<rlim_t>(rlim.rlim_cur, 131072);
		rlim.rlim_cur = std::min<rlim_t>(wanted, rlim.rlim_max);
		setrlimit(RLIMIT_NOFILE, &rlim);  // Ignore errors.
	}

	conf_prepare(argc, argv);
	if (conf_prune_bind_mounts) {
		bind_mount_init();
	}

	int fd = open(conf_output.c_str(), O_RDONLY);
	ExistingDB existing_db(fd);

	DictionaryBuilder dict_builder(/*blocks_to_keep=*/1000, conf_block_size);

	gid_t owner = -1;
	if (conf_check_visibility) {
		group *grp = getgrnam(GROUPNAME);
		if (grp == nullptr) {
			fprintf(stderr, "Unknown group %s\n", GROUPNAME);
			exit(1);
		}
		owner = grp->gr_gid;
	}

	DatabaseBuilder db(conf_output.c_str(), owner, conf_block_size, existing_db.read_next_dictionary(), conf_check_visibility);
	db.set_conf_block(conf_block);
	DatabaseReceiver *corpus = db.start_corpus(/*store_dir_times=*/true, /*store_metadata=*/true);

	int root_fd = opendir_noatime(AT_FDCWD, conf_scan_root);
	if (root_fd == -1) {
		perror(".");
		exit(1);
	}

	struct stat buf;
	if (fstat(root_fd, &buf) == -1) {
		perror(".");
		exit(1);
	}

	vector<HistoryEvent> history_events;
	scan(conf_scan_root, root_fd, buf.st_dev, get_dirtime_from_stat(buf), /*db_modified=*/unknown_dir_time, &existing_db, corpus, &dict_builder, &history_events);

	// It's too late to use the dictionary for the data we already compressed,
	// unless we wanted to either scan the entire file system again (acceptable
	// for plocate-build where it's cheap, less so for us), or uncompressing
	// and recompressing. Instead, we store it for next time, assuming that the
	// data changes fairly little from time to time.
	string next_dictionary = dict_builder.train(1024);
	db.set_next_dictionary(next_dictionary);
	db.set_history_events(move(history_events));
	db.finish_corpus();

	exit(EXIT_SUCCESS);
}
