/* Uniblox - the small C library Lua is built against (see libc/README.md).
 *
 * Compiled with src/script/libc first on the include path, like Lua.
 * Maths uses x87 instructions (all on the original Pentium; also valid in
 * 64-bit mode, so the host tests run exactly this code). Number formatting
 * and parsing work in 80-bit long double, enough for the 14 significant
 * digits Lua prints. */
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ub_types.h"
#include "platform/platform.h"

int ubc_errno;

/* ------------------------------------------------------------ strings */

void *ubc_memchr(const void *p, int c, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i)
        if (s[i] == (unsigned char)c) return (void *)(s + i);
    return NULL;
}

size_t ubc_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int ubc_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int ubc_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

int ubc_strcoll(const char *a, const char *b) { return ubc_strcmp(a, b); }

char *ubc_strchr(const char *s, int c)
{
    for (;; ++s) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *ubc_strpbrk(const char *s, const char *set)
{
    for (; *s; ++s)
        if (ubc_strchr(set, *s)) return (char *)s;
    return NULL;
}

size_t ubc_strspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && ubc_strchr(set, s[n])) ++n;
    return n;
}

size_t ubc_strcspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && !ubc_strchr(set, s[n])) ++n;
    return n;
}

char *ubc_strstr(const char *s, const char *sub)
{
    size_t n = ubc_strlen(sub);
    if (!n) return (char *)s;
    for (; *s; ++s)
        if (*s == *sub && memcmp(s, sub, n) == 0) return (char *)s;
    return NULL;
}

char *ubc_strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *ubc_strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}

char *ubc_strcat(char *d, const char *s)
{
    ubc_strcpy(d + ubc_strlen(d), s);
    return d;
}

char *ubc_strncat(char *d, const char *s, size_t n)
{
    char *e = d + ubc_strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; ++i) e[i] = s[i];
    e[i] = 0;
    return d;
}

char *ubc_strerror(int e)
{
    return (char *)(e == ENOENT ? "file not found" : "error");
}

struct lconv *ubc_localeconv(void)
{
    static char point[] = ".";
    static struct lconv c = { point };
    return &c;
}

/* ------------------------------------------------------------- memory */

/* A size header in front of each block, since the platform has no realloc.
 * (Lua itself uses the game's own allocator; this is for lauxlib.) */
void *ubc_realloc(void *p, size_t n)
{
    size_t old = p ? ((size_t *)p)[-1] : 0;
    if (n == 0) {
        ubc_free(p);
        return NULL;
    }
    size_t *q = (size_t *)plat_alloc(n + sizeof(size_t) * 2);
    if (!q) return NULL;
    q[0] = n;
    void *r = q + 2;
    if (p) memcpy(r, p, old < n ? old : n);
    ubc_free(p);
    return r;
}

void ubc_free(void *p)
{
    if (p) plat_free((size_t *)p - 2);
}

void ubc_exit(int code)
{
    (void)code;
    plat_debug_output("Lua called exit(); stopping\n");
    plat_request_quit();
    for (;;) {}
}

void ubc_abort(void) { ubc_exit(1); }

static u32 g_rand_seed = 1;
int ubc_rand(void)
{
    g_rand_seed = g_rand_seed * 1103515245u + 12345u;
    return (int)((g_rand_seed >> 16) & RAND_MAX);
}
void ubc_srand(unsigned seed) { g_rand_seed = seed; }

/* ------------------------------------------------------------- maths */

static double nan_value(void) { return __builtin_nan(""); }

/* The x87 precision setting differs by OS: Windows runs it at 53 bits
 * (double), Linux at 64 (extended). Functions that need one or the other
 * set it and put the caller's back. */
#define FPU_DOUBLE 0x200
#define FPU_EXTENDED 0x300
static unsigned short fpu_set(unsigned short pc)
{
    unsigned short old, cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    cw = (unsigned short)((old & ~0x300) | pc);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    return old;
}
static void fpu_restore(unsigned short old) { __asm__ __volatile__("fldcw %0" : : "m"(old)); }
static int is_nan(double x) { return x != x; }
static int is_inf(double x) { return x == HUGE_VAL || x == -HUGE_VAL; }

double ubc_fabs(double x) { return x < 0 ? -x : (x == 0 ? 0.0 : x); }

double ubc_floor(double x)
{
    if (is_nan(x) || ubc_fabs(x) >= 4503599627370496.0) return x; /* 2^52: already whole */
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

double ubc_ceil(double x)
{
    if (is_nan(x) || ubc_fabs(x) >= 4503599627370496.0) return x;
    double t = (double)(long long)x;
    return t < x ? t + 1.0 : t;
}

double ubc_modf(double x, double *ip)
{
    double i = x < 0 ? ubc_ceil(x) : ubc_floor(x);
    *ip = i;
    if (is_inf(x)) return x < 0 ? -0.0 : 0.0;
    return x - i;
}

typedef union {
    double d;
    u64 u;
} dbits;

double ubc_frexp(double x, int *e)
{
    dbits b;
    b.d = x;
    int ex = (int)((b.u >> 52) & 0x7FF);
    if (x == 0 || is_nan(x) || is_inf(x)) {
        *e = 0;
        return x;
    }
    if (ex == 0) { /* subnormal: scale up first */
        b.d = x * 18014398509481984.0; /* 2^54 */
        ex = (int)((b.u >> 52) & 0x7FF) - 54;
    }
    *e = ex - 1022;
    b.u = (b.u & ~((u64)0x7FF << 52)) | ((u64)1022 << 52);
    return b.d;
}

double ubc_ldexp(double x, int e)
{
    if (x == 0 || is_nan(x) || is_inf(x)) return x;
    /* scale in steps so no intermediate over/underflows needlessly */
    while (e > 1000) { x *= 1.0715086071862673e301; e -= 1000; }  /* 2^1000 */
    while (e < -1000) { x *= 9.332636185032189e-302; e += 1000; } /* 2^-1000 */
    dbits b;
    b.u = (u64)(e + 1023) << 52;
    if (e >= -1022) return x * b.d;
    return x * 2.2250738585072014e-308 * ubc_ldexp(1.0, e + 1022);
}

double ubc_sqrt(double x)
{
    if (x < 0) return nan_value();
    unsigned short cw = fpu_set(FPU_DOUBLE); /* rounded once, straight to double */
    __asm__ __volatile__("fsqrt" : "=t"(x) : "0"(x));
    fpu_restore(cw);
    return x;
}

/* fsin/fcos/fptan only reduce arguments below 2^63 */
#define TRIG_LIMIT 9.2e18

double ubc_sin(double x)
{
    if (!(ubc_fabs(x) < TRIG_LIMIT)) return nan_value();
    __asm__("fsin" : "=t"(x) : "0"(x));
    return x;
}

double ubc_cos(double x)
{
    if (!(ubc_fabs(x) < TRIG_LIMIT)) return nan_value();
    __asm__("fcos" : "=t"(x) : "0"(x));
    return x;
}

double ubc_tan(double x)
{
    if (!(ubc_fabs(x) < TRIG_LIMIT)) return nan_value();
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(x) : "0"(x));
    return x;
}

double ubc_atan2(double y, double x)
{
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double ubc_atan(double x) { return ubc_atan2(x, 1.0); }

double ubc_asin(double x)
{
    if (x > 1 || x < -1) return nan_value();
    return ubc_atan2(x, ubc_sqrt((1.0 - x) * (1.0 + x)));
}

double ubc_acos(double x)
{
    if (x > 1 || x < -1) return nan_value();
    return ubc_atan2(ubc_sqrt((1.0 - x) * (1.0 + x)), x);
}

/* 2^x for finite x, in long double */
static long double exp2l_x87(long double x)
{
    long double ip, fr;
    /* st0 = x -> st0 = 2^frac - 1, st1 = whole part */
    __asm__("fld %%st(0)\n\tfrndint\n\tfxch\n\tfsub %%st(1), %%st\n\tf2xm1" : "=t"(fr), "=u"(ip) : "0"(x));
    fr += 1.0L;
    __asm__("fscale" : "=t"(fr) : "0"(fr), "u"(ip));
    return fr;
}

double ubc_exp(double x)
{
    if (is_nan(x)) return x;
    if (x > 709.8) return HUGE_VAL;
    if (x < -745.2) return 0.0;
    unsigned short cw = fpu_set(FPU_EXTENDED);
    double r = (double)exp2l_x87((long double)x * 1.4426950408889634073599L);
    fpu_restore(cw);
    return r;
}

/* log2(x) * k for x > 0 */
static long double ylog2(long double x, long double k)
{
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(k) : "st(1)");
    return r;
}

double ubc_log(double x)
{
    if (is_nan(x) || x < 0) return nan_value();
    if (x == 0) return -HUGE_VAL;
    if (is_inf(x)) return x;
    unsigned short cw = fpu_set(FPU_EXTENDED);
    double r = (double)ylog2(x, 0.6931471805599453094172L);
    fpu_restore(cw);
    return r;
}

double ubc_log10(double x)
{
    if (is_nan(x) || x < 0) return nan_value();
    if (x == 0) return -HUGE_VAL;
    if (is_inf(x)) return x;
    unsigned short cw = fpu_set(FPU_EXTENDED);
    double r = (double)ylog2(x, 0.3010299956639811952137L);
    fpu_restore(cw);
    return r;
}

double ubc_pow(double x, double y)
{
    if (y == 0 || x == 1) return 1.0;
    if (is_nan(x) || is_nan(y)) return nan_value();
    int y_int = y == ubc_floor(y) && ubc_fabs(y) < 9.0e15;
    int y_odd = y_int && ubc_fmod(ubc_fabs(y), 2.0) == 1.0;
    if (is_inf(y)) {
        double ax = ubc_fabs(x);
        if (ax == 1) return 1.0;
        return (ax > 1) == (y > 0) ? HUGE_VAL : 0.0;
    }
    if (x == 0) {
        if (y > 0) return y_odd ? x : 0.0;
        return y_odd && 1 / x < 0 ? -HUGE_VAL : HUGE_VAL;
    }
    if (x < 0 && !y_int) return nan_value();
    double ax = ubc_fabs(x), r;
    unsigned short cw = fpu_set(FPU_EXTENDED);
    if (y_int && ubc_fabs(y) <= 1024) { /* exact by squaring */
        long double b = ax, acc = 1;
        unsigned long long n = (unsigned long long)ubc_fabs(y);
        while (n) {
            if (n & 1) acc *= b;
            b *= b;
            n >>= 1;
        }
        r = (double)(y < 0 ? 1.0L / acc : acc);
    } else {
        long double l = ylog2(ax, y);       /* y * log2|x| */
        if (l > 1024) r = HUGE_VAL;
        else if (l < -1080) r = 0.0;
        else r = (double)exp2l_x87(l);
    }
    fpu_restore(cw);
    return x < 0 && y_odd ? -r : r;
}

double ubc_fmod(double x, double y)
{
    if (is_nan(x) || is_nan(y) || is_inf(x) || y == 0) return nan_value();
    if (is_inf(y)) return x;
    double r;
    __asm__("1:\n\tfprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

double ubc_sinh(double x)
{
    double e = ubc_exp(ubc_fabs(x));
    double r = 0.5 * (e - 1.0 / e);
    return x < 0 ? -r : r;
}

double ubc_cosh(double x)
{
    double e = ubc_exp(ubc_fabs(x));
    return 0.5 * (e + 1.0 / e);
}

double ubc_tanh(double x)
{
    if (ubc_fabs(x) > 20) return x < 0 ? -1.0 : 1.0;
    double e = ubc_exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}

/* ---------------------------------------------------------- setjmp */

#if defined(_WIN32) && defined(__i386__)
/* ubc_setjmp(buf): saves the callee-saved registers, the stack pointer as
 * it will be after returning, and the return address. */
__asm__(".text\n"
        ".globl _ubc_setjmp\n"
        "_ubc_setjmp:\n"
        "  movl 4(%esp), %eax\n"
        "  movl %ebx, 0(%eax)\n"
        "  movl %esi, 4(%eax)\n"
        "  movl %edi, 8(%eax)\n"
        "  movl %ebp, 12(%eax)\n"
        "  leal 4(%esp), %ecx\n"
        "  movl %ecx, 16(%eax)\n"
        "  movl (%esp), %ecx\n"
        "  movl %ecx, 20(%eax)\n"
        "  xorl %eax, %eax\n"
        "  ret\n"
        ".globl _ubc_longjmp\n"
        "_ubc_longjmp:\n"
        "  movl 4(%esp), %edx\n"
        "  movl 8(%esp), %eax\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  incl %eax\n"
        "1:\n"
        "  movl 0(%edx), %ebx\n"
        "  movl 4(%edx), %esi\n"
        "  movl 8(%edx), %edi\n"
        "  movl 12(%edx), %ebp\n"
        "  movl 16(%edx), %esp\n"
        "  jmp *20(%edx)\n");
#endif

/* ---------------------------------------------------------- numbers */

static long double pow10l(int e)
{
    long double r = 1, b = 10;
    unsigned n = (unsigned)(e < 0 ? -e : e);
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1 / r : r;
}

/* Decimal only (Lua handles hexadecimal itself with strtoul). No "inf" or
 * "nan": tonumber("nan") is nil, as everywhere in Roblox. */
double ubc_strtod(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    u64 mant = 0;
    int digits = 0, exp10 = 0, any = 0;
    for (; isdigit((unsigned char)*p); ++p, any = 1) {
        if (digits < 19) {
            mant = mant * 10 + (u64)(*p - '0');
            if (mant) digits++;
        } else {
            exp10++;
        }
    }
    if (*p == '.') {
        ++p;
        for (; isdigit((unsigned char)*p); ++p, any = 1) {
            if (digits < 19) {
                mant = mant * 10 + (u64)(*p - '0');
                if (mant) digits++;
                exp10--;
            }
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, ev = 0, edig = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        for (; isdigit((unsigned char)*q); ++q, ++edig)
            if (ev < 100000) ev = ev * 10 + (*q - '0');
        if (edig) {
            p = q;
            exp10 += eneg ? -ev : ev;
        }
    }
    if (end) *end = (char *)p;
    unsigned short cw = fpu_set(FPU_EXTENDED);
    long double v = (long double)mant;
    if (mant && exp10) {
        if (exp10 > 330 + 19) v = HUGE_VAL;
        else if (exp10 < -360) v = 0;
        else v = exp10 < 0 ? v / pow10l(-exp10) : v * pow10l(exp10);
    }
    double d = (double)v;
    fpu_restore(cw);
    if (is_inf(d)) ubc_errno = ERANGE;
    return neg ? -d : d;
}

unsigned long ubc_strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    const char *start = p;
    for (;; ++p) {
        int c = (unsigned char)*p, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? 0ul - v : v;
}

/* ------------------------------------------------------------ printf */

typedef struct {
    char *buf;
    size_t cap, len;
} out;

static void put(out *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void put_pad(out *o, char c, int n)
{
    while (n-- > 0) put(o, c);
}

/* Exact decimal digits of a double, with big integers: the value is
 * num / den, scaled so the first digit is num / den. Correctly rounded
 * (ties to even), so output matches a good C library digit for digit. */

#define BIG_WORDS 48 /* 1536 bits: room for 2^1078 * 10 */

typedef struct {
    int n;          /* words used */
    u32 w[BIG_WORDS];
} big;

static void big_set(big *b, u64 v)
{
    b->w[0] = (u32)v;
    b->w[1] = (u32)(v >> 32);
    b->n = b->w[1] ? 2 : b->w[0] ? 1 : 0;
}

static void big_mul_small(big *b, u32 m)
{
    u64 carry = 0;
    for (int i = 0; i < b->n; ++i) {
        u64 t = (u64)b->w[i] * m + carry;
        b->w[i] = (u32)t;
        carry = t >> 32;
    }
    if (carry && b->n < BIG_WORDS) b->w[b->n++] = (u32)carry;
}

static void big_shl(big *b, int bits)
{
    int words = bits / 32, r = bits % 32;
    if (b->n == 0) return;
    if (r) {
        u32 carry = 0;
        for (int i = 0; i < b->n; ++i) {
            u32 nw = (b->w[i] << r) | carry;
            carry = b->w[i] >> (32 - r);
            b->w[i] = nw;
        }
        if (carry && b->n < BIG_WORDS) b->w[b->n++] = carry;
    }
    if (words) {
        int nn = b->n + words > BIG_WORDS ? BIG_WORDS : b->n + words;
        for (int i = nn - 1; i >= words; --i) b->w[i] = b->w[i - words];
        for (int i = 0; i < words; ++i) b->w[i] = 0;
        b->n = nn;
    }
}

static int big_cmp(const big *a, const big *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; --i)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static void big_sub(big *a, const big *b) /* a -= b, a >= b */
{
    i64 borrow = 0;
    for (int i = 0; i < a->n; ++i) {
        i64 t = (i64)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
        borrow = t < 0;
        a->w[i] = (u32)(t + (borrow ? ((i64)1 << 32) : 0));
    }
    while (a->n && !a->w[a->n - 1]) a->n--;
}

static void big_pow10(big *b, int k)
{
    while (k >= 9) { big_mul_small(b, 1000000000u); k -= 9; }
    while (k-- > 0) big_mul_small(b, 10);
}

/* Digits of v > 0. mode 'e': n significant digits; mode 'f': digits up to n
 * places after the point (n may be negative). Returns the digit count and
 * sets *e to the decimal exponent of the first digit (after rounding). */
static int exact_digits(double v, char mode, int n, char *dig, int max, int *e)
{
    int bexp;
    double f = ubc_frexp(v, &bexp);                 /* v = f * 2^bexp, 0.5 <= f < 1 */
    u64 m = (u64)ubc_ldexp(f, 53);
    int sh = bexp - 53;                             /* v = m * 2^sh */
    big num, den;
    big_set(&num, m);
    big_set(&den, 1);
    if (sh >= 0) big_shl(&num, sh);
    else big_shl(&den, -sh);
    int k = (int)((double)(bexp - 1) * 0.30102999566398119521); /* ~ floor(log10 v) */
    if (k >= 0) big_pow10(&den, k);
    else big_pow10(&num, -k);
    /* now num/den is near [1, 10): fix up the estimate */
    big t = den;
    big_mul_small(&t, 10);
    while (big_cmp(&num, &t) >= 0) { big_mul_small(&den, 10); k++; t = den; big_mul_small(&t, 10); }
    while (big_cmp(&num, &den) < 0) { big_mul_small(&num, 10); k--; }
    int count = mode == 'e' ? n : k + 1 + n;
    if (count > max) count = max;
    if (count <= 0) {
        /* nothing before the rounding position: 0, or 1 in the last place */
        *e = k;
        if (count < 0) return 0;
        /* count == 0: round num/den * 10^0 against half of 10 (the first digit's place + 1) */
        big h = den;
        big_mul_small(&h, 5);
        int c = big_cmp(&num, &h);
        if (c > 0) {
            dig[0] = '1';
            *e = k + 1;
            return 1;
        }
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        int d = 0;
        while (big_cmp(&num, &den) >= 0) { big_sub(&num, &den); d++; }
        dig[i] = (char)('0' + d);
        big_mul_small(&num, 10);
    }
    /* remainder*10 vs 5*den: above half, half, or below */
    big h = den;
    big_mul_small(&h, 5);
    int c = big_cmp(&num, &h);
    if (c > 0 || (c == 0 && ((dig[count - 1] - '0') & 1))) {
        int i = count - 1;
        while (i >= 0 && dig[i] == '9') dig[i--] = '0';
        if (i >= 0) {
            dig[i]++;
        } else { /* 99.9 -> 100.0: one more digit in front */
            dig[0] = '1';
            for (int j = 1; j < count; ++j) dig[j] = '0';
            k++;
            if (mode == 'f' && count < max) dig[count++] = '0';
        }
    }
    *e = k;
    return count;
}

#define FMT_LEFT 1
#define FMT_PLUS 2
#define FMT_SPACE 4
#define FMT_ALT 8
#define FMT_ZERO 16

static void emit_field(out *o, const char *body, int len, const char *sign, int width, int flags)
{
    int sl = (int)ubc_strlen(sign), pad = width - len - sl;
    if (!(flags & FMT_LEFT) && !(flags & FMT_ZERO)) put_pad(o, ' ', pad);
    for (int i = 0; i < sl; ++i) put(o, sign[i]);
    if (!(flags & FMT_LEFT) && (flags & FMT_ZERO)) put_pad(o, '0', pad);
    for (int i = 0; i < len; ++i) put(o, body[i]);
    if (flags & FMT_LEFT) put_pad(o, ' ', pad);
}

#define DIG_MAX 400

static void fmt_double(out *o, double v, int prec, char conv, int width, int flags)
{
    char body[DIG_MAX + 40];
    int n = 0;
    const char *sign = v < 0 || (v == 0 && 1 / v < 0) ? "-" : (flags & FMT_PLUS) ? "+" : (flags & FMT_SPACE) ? " " : "";
    int upper = conv == 'E' || conv == 'G' || conv == 'F';
    if (is_nan(v) || is_inf(v)) {
        const char *t = is_nan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        if (is_nan(v)) sign = "";
        emit_field(o, t, 3, sign, width, flags & ~FMT_ZERO);
        return;
    }
    double a = v < 0 ? -v : v;
    if (prec < 0) prec = 6;
    if (tolower(conv) == 'g' && !(flags & FMT_ALT) && a < 1e15 && a == (double)(u64)a &&
        (a < 10 || a < (double)pow10l(prec == 0 ? 1 : prec))) {
        /* whole numbers, the usual case: no need for the exact method */
        char t[24];
        int k = 0;
        u64 u = (u64)a;
        do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (k > 0) body[n++] = t[--k];
        emit_field(o, body, n, sign, width, flags);
        return;
    }
    if (prec > DIG_MAX - 40) prec = DIG_MAX - 40;
    char dig[DIG_MAX + 2];
    int e = 0, nd;
    char c = (char)tolower(conv);
    int strip = 0;
    if (c == 'g') {
        int p = prec == 0 ? 1 : prec;
        if (a != 0) exact_digits(a, 'e', p, dig, DIG_MAX, &e);
        if (e < -4 || e >= p) {
            c = 'e';
            prec = p - 1;
        } else {
            c = 'f';
            prec = p - 1 - e;
        }
        strip = !(flags & FMT_ALT);
    }
    if (c == 'e') {
        if (a == 0) {
            e = 0;
            for (int i = 0; i <= prec; ++i) dig[i] = '0';
        } else {
            exact_digits(a, 'e', prec + 1, dig, DIG_MAX, &e);
        }
        body[n++] = dig[0];
        if (prec > 0 || (flags & FMT_ALT)) body[n++] = '.';
        for (int i = 1; i <= prec; ++i) body[n++] = dig[i];
        if (strip) {
            while (n > 1 && body[n - 1] == '0') n--;
            if (body[n - 1] == '.') n--;
        }
        body[n++] = upper ? 'E' : 'e';
        body[n++] = e < 0 ? '-' : '+';
        int ae = e < 0 ? -e : e;
        if (ae >= 100) body[n++] = (char)('0' + ae / 100);
        body[n++] = (char)('0' + (ae / 10) % 10);
        body[n++] = (char)('0' + ae % 10);
    } else { /* 'f': digits up to prec places after the point */
        nd = a == 0 ? 0 : exact_digits(a, 'f', prec, dig, DIG_MAX, &e);
        /* dig[i] has place value 10^(e - i) */
        int int_places = nd > 0 && e >= 0 ? e + 1 : 1;
        for (int pl = int_places - 1; pl >= 0 && n < DIG_MAX + 30; --pl) {
            int i = e - pl;
            body[n++] = nd > 0 && i >= 0 && i < nd ? dig[i] : '0';
        }
        if (prec > 0 || (flags & FMT_ALT)) body[n++] = '.';
        for (int pl = 1; pl <= prec && n < DIG_MAX + 30; ++pl) {
            int i = e + pl;
            body[n++] = nd > 0 && i >= 0 && i < nd ? dig[i] : '0';
        }
        if (strip && ubc_memchr(body, '.', (size_t)n)) {
            while (body[n - 1] == '0') n--;
            if (body[n - 1] == '.') n--;
        }
    }
    emit_field(o, body, n, sign, width, flags);
}

static void fmt_int(out *o, unsigned long long v, int neg, int base, int upper, int prec, int width, int flags)
{
    char body[80];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[70];
    int t = 0, nonzero = v != 0;
    while (v) {
        tmp[t++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    if (prec < 0) prec = 1;
    else flags &= ~FMT_ZERO;
    for (int i = t; i < prec && n < 60; ++i) body[n++] = '0';
    if ((flags & FMT_ALT) && base == 8 && (t == 0 || prec <= t) && n == 0) body[n++] = '0';
    while (t > 0) body[n++] = tmp[--t];
    const char *sign = neg ? "-" : (flags & FMT_PLUS) ? "+" : (flags & FMT_SPACE) ? " " : "";
    if ((flags & FMT_ALT) && base == 16 && nonzero) sign = upper ? "0X" : "0x";
    emit_field(o, body, n, sign, width, flags);
}

int ubc_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    out o = { buf, cap, 0 };
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        ++p;
        int flags = 0, width = 0, prec = -1;
        for (;; ++p) {
            if (*p == '-') flags |= FMT_LEFT;
            else if (*p == '+') flags |= FMT_PLUS;
            else if (*p == ' ') flags |= FMT_SPACE;
            else if (*p == '#') flags |= FMT_ALT;
            else if (*p == '0') flags |= FMT_ZERO;
            else break;
        }
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= FMT_LEFT; width = -width; }
            ++p;
        } else {
            while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            ++p;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                ++p;
            } else {
                while (isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0');
            }
        }
        if (flags & FMT_LEFT) flags &= ~FMT_ZERO;
        int lng = 0; /* 0 int, 1 long, 2 long long, -1 size_t */
        if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'l') { ++p; lng = 1; if (*p == 'l') { ++p; lng = 2; } }
        else if (*p == 'z') { ++p; lng = -1; }
        else if (*p == 'L') { ++p; }
        char c = *p;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long) : lng == 1 ? (long long)va_arg(ap, long)
                        : lng == -1 ? (long long)va_arg(ap, size_t) : (long long)va_arg(ap, int);
            unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            fmt_int(&o, u, v < 0, 10, 0, prec, width, flags);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng == 2 ? va_arg(ap, unsigned long long) : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long)
                                 : lng == -1 ? (unsigned long long)va_arg(ap, size_t) : (unsigned long long)va_arg(ap, unsigned);
            fmt_int(&o, v, 0, c == 'o' ? 8 : c == 'u' ? 10 : 16, c == 'X', prec, width, flags & ~(FMT_PLUS | FMT_SPACE));
            break;
        }
        case 'p': {
            void *v = va_arg(ap, void *);
            fmt_int(&o, (unsigned long long)(size_t)v, 0, 16, 0, -1, width, (flags | FMT_ALT) & ~FMT_ZERO);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            emit_field(&o, &ch, 1, "", width, flags & ~FMT_ZERO);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len] && (prec < 0 || len < prec)) ++len;
            emit_field(&o, s, len, "", width, flags & ~FMT_ZERO);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
            fmt_double(&o, va_arg(ap, double), prec, c, width, flags);
            break;
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            put(&o, c);
            break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int ubc_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = ubc_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* Lua only uses sprintf into buffers it sized for the format; cap at 512
 * (LUAI_MAXNUMBER2STR is 32 and string.format's buffer is 512). */
int ubc_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = ubc_vsnprintf(buf, 512, fmt, ap);
    va_end(ap);
    return r;
}

/* -------------------------------------------------------------- stdio */

struct ubc_file {
    int which;   /* 1 stdout, 2 stderr */
    char line[512];
    int len;
};

static struct ubc_file g_out = { 1, { 0 }, 0 }, g_err = { 2, { 0 }, 0 };
FILE *ubc_stdin = NULL, *ubc_stdout = &g_out, *ubc_stderr = &g_err;
static void (*g_output)(const char *line, int is_error);

void ubc_set_output(void (*fn)(const char *line, int is_error)) { g_output = fn; }

static void flush_line(FILE *f)
{
    if (!f || !f->len) return;
    f->line[f->len] = 0;
    if (g_output) g_output(f->line, f->which == 2);
    else plat_debug_output(f->line);
    f->len = 0;
}

int ubc_fputc(int c, FILE *f)
{
    if (!f) return EOF;
    if (c == '\n') {
        flush_line(f);
    } else {
        if (f->len >= (int)sizeof f->line - 1) flush_line(f);
        f->line[f->len++] = (char)c;
    }
    return c;
}

int ubc_fputs(const char *s, FILE *f)
{
    while (*s) ubc_fputc((unsigned char)*s++, f);
    return 0;
}

int ubc_fprintf(FILE *f, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int r = ubc_vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ubc_fputs(b, f);
    return r;
}

int ubc_fflush(FILE *f)
{
    flush_line(f);
    return 0;
}

FILE *ubc_fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    ubc_errno = ENOENT;
    return NULL;
}

FILE *ubc_freopen(const char *path, const char *mode, FILE *f)
{
    (void)f;
    return ubc_fopen(path, mode);
}

int ubc_fclose(FILE *f) { (void)f; return 0; }
size_t ubc_fread(void *p, size_t size, size_t n, FILE *f) { (void)p; (void)size; (void)n; (void)f; return 0; }
int ubc_getc(FILE *f) { (void)f; return EOF; }
int ubc_ungetc(int c, FILE *f) { (void)c; (void)f; return EOF; }
int ubc_feof(FILE *f) { (void)f; return 1; }
int ubc_ferror(FILE *f) { (void)f; return 0; }
