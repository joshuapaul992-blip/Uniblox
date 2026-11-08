#include "core/ub_string.h"

size_t ub_strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

typedef struct {
    char *buf;
    int size;
    int len;
} ub_out;

static void out_ch(ub_out *o, char c)
{
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void out_uint(ub_out *o, u32 v, unsigned base, bool upper,
                     int width, char pad, bool neg)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0') out_ch(o, '-');
    for (; total < width; ++total) out_ch(o, pad);
    if (neg && pad != '0') out_ch(o, '-');
    while (n) out_ch(o, tmp[--n]);
}

int ub_vfmt(char *buf, int size, const char *fmt, va_list ap)
{
    ub_out o = { buf, size, 0 };
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            out_ch(&o, *p);
            continue;
        }
        ++p;
        char pad = ' ';
        int width = 0;
        if (*p == '0') { pad = '0'; ++p; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        while (*p == 'l') ++p;
        switch (*p) {
        case 'd':
        case 'i': {
            i32 v = va_arg(ap, i32);
            bool neg = v < 0;
            u32 mag = neg ? (u32)0 - (u32)v : (u32)v;
            out_uint(&o, mag, 10, false, width, pad, neg);
        } break;
        case 'u': out_uint(&o, va_arg(ap, u32), 10, false, width, pad, false); break;
        case 'x': out_uint(&o, va_arg(ap, u32), 16, false, width, pad, false); break;
        case 'X': out_uint(&o, va_arg(ap, u32), 16, true, width, pad, false); break;
        case 'c': out_ch(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = (int)ub_strlen(s);
            for (; n < width; ++n) out_ch(&o, ' ');
            while (*s) out_ch(&o, *s++);
        } break;
        case '%': out_ch(&o, '%'); break;
        case '\0': --p; break; /* trailing '%': stop cleanly */
        default: out_ch(&o, '%'); out_ch(&o, *p); break;
        }
    }
    if (size > 0) buf[o.len < size ? o.len : size - 1] = '\0';
    return o.len;
}

int ub_fmt(char *buf, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = ub_vfmt(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
