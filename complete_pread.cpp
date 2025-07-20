#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool try_complete_pread(int fd, void *ptr, size_t len, off_t offset)
{
	while (len > 0) {
		ssize_t ret = pread(fd, ptr, len, offset);
		if (ret == -1 && errno == EINTR) {
			continue;
		}
		if (ret <= 0) {
			return false;
		}
		ptr = reinterpret_cast<char *>(ptr) + ret;
		len -= ret;
		offset += ret;
	}
	return true;
}

void complete_pread(int fd, void *ptr, size_t len, off_t offset, const char *filename_for_errors)
{
	if (!try_complete_pread(fd, ptr, len, offset)) {
		if (errno == 0) {
			fprintf(stderr, "%s: Short read (file corrupted?)\n", filename_for_errors);
		} else {
			perror(filename_for_errors);
		}
		exit(1);
	}
}
