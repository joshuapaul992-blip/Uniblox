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

int ub_fmt_num(char *buf, int size, f32 v, int decimals)
{
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    if (!(v == v)) return ub_fmt(buf, size, "nan");
    u32 scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    f64 a = v < 0 ? -(f64)v : (f64)v;
    if (a > 2.0e9) return ub_fmt(buf, size, "%s%u", v < 0 ? "-" : "", (u32)2000000000u);
    u64 fixed = (u64)(a * (f64)scale + 0.5);
    u32 whole = (u32)(fixed / scale), frac = (u32)(fixed % scale);
    int places = decimals;
    while (places > 0 && frac % 10 == 0) {
        frac /= 10;
        --places;
    }
    bool neg = v < 0 && fixed != 0;
    if (!places) return ub_fmt(buf, size, "%s%u", neg ? "-" : "", whole);
    char f[8];
    for (int i = places - 1; i >= 0; --i) {
        f[i] = (char)('0' + frac % 10);
        frac /= 10;
    }
    f[places] = 0;
    return ub_fmt(buf, size, "%s%u.%s", neg ? "-" : "", whole, f);
}

bool ub_parse_num(const char *s, f32 *out)
{
    while (*s == ' ' || *s == '\t') ++s;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    f64 v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        ++digits;
    }
    if (*s == '.') {
        ++s;
        f64 f = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * f;
            f *= 0.1;
            ++digits;
        }
    }
    if (!digits) return false;
    if (*s == 'e' || *s == 'E') {
        ++s;
        bool eneg = false;
        if (*s == '-' || *s == '+') eneg = *s++ == '-';
        int e = 0, ed = 0;
        while (*s >= '0' && *s <= '9' && e < 1000) {
            e = e * 10 + (*s++ - '0');
            ++ed;
        }
        if (!ed) return false;
        for (int i = 0; i < e && i < 40; ++i) v = eneg ? v / 10 : v * 10;
    }
    while (*s == ' ' || *s == '\t') ++s;
    if (*s) return false;
    if (v > 3.0e38) v = 3.0e38;
    *out = (f32)(neg ? -v : v);
    return true;
}

int ub_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(u8)*a - (int)(u8)*b;
}
