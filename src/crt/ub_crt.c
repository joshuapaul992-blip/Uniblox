/* Uniblox - minimal C runtime.
 *
 * We link with -nostdlib so no Microsoft CRT (msvcrt.dll, crtdll.dll, ucrt)
 * is ever imported. GCC may still emit calls to these four functions for
 * struct copies and zero-initialisation, and the renderer clears a 300 KB
 * z-buffer every frame, so the bulk paths move 4 bytes at a time.
 *
 * This file is built with -fno-tree-loop-distribute-patterns (see Makefile)
 * so GCC cannot "optimise" these loops back into calls to themselves. */
#include <stddef.h>
#include <stdint.h>

/* may_alias: these words overlay memory of any type */
typedef uint32_t __attribute__((__may_alias__)) word;

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
        while (n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
        word *dw = (word *)d;
        const word *sw = (const word *)s;
        while (n >= 16) {
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw += 4; sw += 4; n -= 16;
        }
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n) return memcpy(dst, src, n); /* no harmful overlap */
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    while (n && ((uintptr_t)d & 3)) { *d++ = v; n--; }
    word w = (word)v * 0x01010101u;
    word *dw = (word *)d;
    while (n >= 16) {
        dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w;
        dw += 4; n -= 16;
    }
    while (n >= 4) { *dw++ = w; n -= 4; }
    d = (unsigned char *)dw;
    while (n--) *d++ = v;
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
