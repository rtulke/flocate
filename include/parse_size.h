#ifndef PARSE_SIZE_H
#define PARSE_SIZE_H 1

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/* Parse a byte count with an optional K, M, G or T suffix (powers of 1024);
   return true if OK. Shared by updatedb (HASH_MAX_SIZE / --hash-max-size)
   and flocate-showdiff (--hash-max-size) so both accept the same spellings. */
inline bool parse_size(uint64_t *dest, const char *str)
{
	if (*str == '\0' || *str == '-')
		return false;
	char *end;
	errno = 0;
	unsigned long long value = strtoull(str, &end, 10);
	if (errno != 0 || end == str)
		return false;
	unsigned shift = 0;
	switch (toupper((unsigned char)*end)) {
	case '\0':
		break;
	case 'K':
		shift = 10;
		++end;
		break;
	case 'M':
		shift = 20;
		++end;
		break;
	case 'G':
		shift = 30;
		++end;
		break;
	case 'T':
		shift = 40;
		++end;
		break;
	default:
		return false;
	}
	if (*end != '\0')
		return false;
	if (shift != 0 && value > (~0ULL >> shift))
		return false;
	*dest = value << shift;
	return true;
}

#endif  // PARSE_SIZE_H
