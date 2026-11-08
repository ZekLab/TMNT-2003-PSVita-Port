/* Minimal string.h for tmnt_code.so. */
#ifndef SO_STRING_H
#define SO_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
#endif
