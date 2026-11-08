#include "core/ub_utf.h"

#define REPLACEMENT 0xFFFDu

u32 ub_utf8_decode(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u32 c = p[0];
    if (c == 0) return 0;
    int extra;
    u32 min;
    if (c < 0x80) { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; c &= 0x07; min = 0x10000; }
    else { *s += 1; return REPLACEMENT; }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return REPLACEMENT; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return REPLACEMENT;
    return c;
}

int ub_utf8_encode(u32 cp, char *out)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = REPLACEMENT;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int ub_utf8_to_utf16(const char *src, u16 *dst, int cap)
{
    int n = 0;
    if (cap <= 0) return 0;
    for (;;) {
        u32 c = ub_utf8_decode(&src);
        if (c == 0) break;
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            dst[n++] = (u16)(0xD800 | (c >> 10));
            dst[n++] = (u16)(0xDC00 | (c & 0x3FF));
        } else {
            if (n + 1 >= cap) break;
            dst[n++] = (u16)c;
        }
    }
    dst[n] = 0;
    return n;
}

int ub_utf16_to_utf8(const u16 *src, char *dst, int cap)
{
    int n = 0;
    if (cap <= 0) return 0;
    while (*src) {
        u32 c = *src++;
        if (c >= 0xD800 && c <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (*src++ - 0xDC00);
        }
        char tmp[4];
        int len = ub_utf8_encode(c, tmp);
        if (n + len >= cap) break;
        for (int i = 0; i < len; ++i) dst[n++] = tmp[i];
    }
    dst[n] = 0;
    return n;
}
