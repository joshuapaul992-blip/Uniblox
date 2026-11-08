/* Uniblox - minimal C runtime.
 *
 * We link with -nostdlib so no Microsoft CRT (msvcrt.dll, crtdll.dll, ucrt)
 * is ever imported. GCC may still emit calls to these four functions for
 * struct copies and zero-initialisation, so they must exist.
 *
 * This file is built with -fno-tree-loop-distribute-patterns (see Makefile)
 * so GCC cannot "optimise" these loops back into calls to themselves. */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (; n; --n, ++x, ++y) {
        if (*x != *y) return (int)*x - (int)*y;
    }
    return 0;
}
