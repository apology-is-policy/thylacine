// Byte helpers for freestanding native C. libt provides no memcpy or memset,
// and -fno-builtin keeps the compiler from turning these loops into calls to
// either.
#ifndef THYLA_BYTES_H
#define THYLA_BYTES_H
#include <stddef.h>

static inline void t_bytes_copy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static inline void t_bytes_zero(void *dst, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = 0;
}

#endif
