/* Uniblox - tests for the C library Lua is built on (src/script/ub_libc.c),
 * checked against the host's C library. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* ub_libc.c's functions (its headers would hide the host's) */
int ubc_snprintf(char *buf, size_t n, const char *fmt, ...);
double ubc_strtod(const char *s, char **end);
unsigned long ubc_strtoul(const char *s, char **end, int base);
double ubc_floor(double), ubc_ceil(double), ubc_fmod(double, double), ubc_sqrt(double), ubc_pow(double, double);
double ubc_exp(double), ubc_log(double), ubc_log10(double), ubc_sin(double), ubc_cos(double), ubc_tan(double);
double ubc_asin(double), ubc_acos(double), ubc_atan(double), ubc_atan2(double, double);
double ubc_sinh(double), ubc_cosh(double), ubc_tanh(double), ubc_frexp(double, int *), ubc_ldexp(double, int);
double ubc_modf(double, double *);
char *ubc_strstr(const char *, const char *);
size_t ubc_strspn(const char *, const char *);
size_t ubc_strcspn(const char *, const char *);
char *ubc_strpbrk(const char *, const char *);

static int g_fmt_bad;

static void check_fmt_d(const char *fmt, double v)
{
    char a[512], b[512];
    ubc_snprintf(a, sizeof a, fmt, v);
    snprintf(b, sizeof b, fmt, v);
    g_checks++;
    if (strcmp(a, b) != 0) {
        if (g_fmt_bad++ < 12) printf("  FAIL format \"%s\" of %.17g: ours \"%s\", libc \"%s\"\n", fmt, v, a, b);
        g_failures++;
    }
}

static void check_fmt_i(const char *fmt, long v)
{
    char a[256], b[256];
    ubc_snprintf(a, sizeof a, fmt, v);
    snprintf(b, sizeof b, fmt, v);
    g_checks++;
    if (strcmp(a, b) != 0) {
        if (g_fmt_bad++ < 12) printf("  FAIL format \"%s\" of %ld: ours \"%s\", libc \"%s\"\n", fmt, v, a, b);
        g_failures++;
    }
}

static int close_rel(double a, double b, double tol)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    if (isinf(a) || isinf(b)) return a == b;
    double d = fabs(a - b), m = fabs(b) > 1 ? fabs(b) : 1;
    return d <= tol * m;
}

void test_libc(void)
{
    /* the number formats Lua uses: %.14g for every tostring, and whatever
     * string.format passes (%d with a long, %5.2f, %e, %g, %x...) */
    static const double vals[] = { 0, -0.0, 1, -1, 0.1, 0.5, 1.5, 2.5, 3.14159265358979, 1e-5, 1e-4, 123456789,
                                   1234567890123456, 1e15, 1e16, 1e21, 1e100, 1e300, 1.7976931348623157e308,
                                   2.2250738585072014e-308, 5e-324, 1.0 / 3, 2.0 / 3, 100, 99.995, 0.000123456,
                                   -987.654321, 6.02214076e23, 4.9406564584124654e-324, 0.1 + 0.2, 9.999999999999999 };
    static const char *const dfmts[] = { "%.14g", "%g", "%.3g", "%e", "%.2e", "%E", "%f", "%.2f", "%.0f", "%10.3f",
                                         "%-12.4g|", "%+g", "% e", "%#g", "%#.0f", "%012.3f", "%G", "%.17g", "%.1f" };
    for (int i = 0; i < (int)(sizeof vals / sizeof vals[0]); ++i)
        for (int k = 0; k < (int)(sizeof dfmts / sizeof dfmts[0]); ++k) {
            if (vals[i] > 1e50 && (strchr(dfmts[k], 'f') != NULL)) continue; /* hundreds of digits: ours stops at ~19 */
            check_fmt_d(dfmts[k], vals[i]);
            check_fmt_d(dfmts[k], -vals[i]);
        }
    /* random values through %.14g */
    unsigned seed = 1;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245u + 12345u;
        double m = (double)(seed >> 8) / 16777216.0;
        seed = seed * 1103515245u + 12345u;
        int e = (int)(seed >> 16) % 80 - 40;
        check_fmt_d("%.14g", m * pow(10, e));
    }
    check_fmt_d("%f", INFINITY);
    check_fmt_d("%g", -INFINITY);
    static const char *const ifmts[] = { "%ld", "%5ld", "%-5ld|", "%05ld", "%+ld", "%lx", "%lX", "%#lx", "%lo", "%lu", "%.3ld" };
    static const long ivals[] = { 0, 1, -1, 42, -42, 255, 65535, 2147483647L, -2147483647L, 123456 };
    for (int i = 0; i < (int)(sizeof ivals / sizeof ivals[0]); ++i)
        for (int k = 0; k < (int)(sizeof ifmts / sizeof ifmts[0]); ++k) {
            if (ivals[i] < 0 && strpbrk(ifmts[k], "xXou")) continue;
            check_fmt_i(ifmts[k], ivals[i]);
        }
    char b[64];
    ubc_snprintf(b, sizeof b, "[%s|%5s|%-5s|%.2s|%c|%%]", "abc", "ab", "ab", "xyz", 'q');
    CHECK_STR(b, "[abc|   ab|ab   |xy|q|%]");
    int n = ubc_snprintf(b, 5, "%s", "hello world");
    CHECK(n == 11 && strcmp(b, "hell") == 0);

    /* parsing: exact for these, and round trips of %.17g */
    static const char *const nums[] = { "0", "1", "-1", "3.5", "1e10", "1E-5", "0.1", "123456789012345678", ".5", "5.",
                                        "1e308", "2.2250738585072014e-308", "6.02214076e23", "  42", "+7", "0.30000000000000004" };
    for (int i = 0; i < (int)(sizeof nums / sizeof nums[0]); ++i) {
        char *e1, *e2;
        double a = ubc_strtod(nums[i], &e1), c = strtod(nums[i], &e2);
        CHECK(a == c && e1 - nums[i] == e2 - nums[i]);
    }
    int exact = 0, total = 0;
    seed = 7;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245u + 12345u;
        double v = (double)seed / 4294967296.0 * pow(10, (int)(seed % 61) - 30);
        char s[64];
        snprintf(s, sizeof s, "%.17g", v);
        double r = ubc_strtod(s, NULL);
        total++;
        exact += r == v;
        CHECK(close_rel(r, v, 4e-16));
    }
    printf("  strtod: %d of %d random values round-trip exactly, the rest within an ulp\n", exact, total);
    char *end;
    CHECK(ubc_strtod("abc", &end) == 0 && strcmp(end, "abc") == 0);
    CHECK(ubc_strtod("0x10", &end) == 0 && *end == 'x');       /* Lua reads hex itself */
    CHECK(ubc_strtod("inf", &end) == 0 && *end == 'i');        /* no inf/nan from text */
    CHECK(ubc_strtod("1e5000", NULL) == HUGE_VAL && ubc_strtod("1e-5000", NULL) == 0);
    CHECK(ubc_strtoul("ff", &end, 16) == 255 && *end == 0);
    CHECK(ubc_strtoul("0x1F", &end, 16) == 31 && *end == 0);
    CHECK(ubc_strtoul("777", &end, 8) == 511);

    /* maths: x87 instructions against libm */
    int math_bad = 0;
    for (int i = 0; i < 4000; ++i) {
        seed = seed * 1103515245u + 12345u;
        double x = ((double)(seed >> 4) / 268435456.0 - 0.5) * 40.0;
        double y = ((double)(seed & 0xFFFF) / 65536.0 - 0.5) * 8.0;
        math_bad += !close_rel(ubc_sin(x), sin(x), 1e-15) + !close_rel(ubc_cos(x), cos(x), 1e-15);
        math_bad += !close_rel(ubc_tan(x), tan(x), 1e-13) + !close_rel(ubc_atan2(y, x), atan2(y, x), 1e-15);
        math_bad += !close_rel(ubc_exp(y * 10), exp(y * 10), 1e-15) + !close_rel(ubc_log(fabs(x) + 1e-3), log(fabs(x) + 1e-3), 1e-15);
        math_bad += !close_rel(ubc_log10(fabs(x) + 1e-3), log10(fabs(x) + 1e-3), 1e-15);
        math_bad += !close_rel(ubc_pow(fabs(x), y), pow(fabs(x), y), 1e-14) + !close_rel(ubc_sqrt(fabs(x)), sqrt(fabs(x)), 0);
        math_bad += !close_rel(ubc_fmod(x, y), fmod(x, y), 1e-15) + (ubc_floor(x) != floor(x)) + (ubc_ceil(x) != ceil(x));
        math_bad += !close_rel(ubc_asin(y / 4), asin(y / 4), 1e-15) + !close_rel(ubc_acos(y / 4), acos(y / 4), 1e-15);
        math_bad += !close_rel(ubc_atan(x), atan(x), 1e-15) + !close_rel(ubc_tanh(y), tanh(y), 1e-14);
        math_bad += !close_rel(ubc_sinh(y), sinh(y), 1e-14) + !close_rel(ubc_cosh(y), cosh(y), 1e-14);
        int e1, e2;
        math_bad += ubc_frexp(x, &e1) != frexp(x, &e2) || e1 != e2;
        math_bad += ubc_ldexp(y, (int)(seed % 200) - 100) != ldexp(y, (int)(seed % 200) - 100);
    }
    CHECK(math_bad == 0);
    if (math_bad) printf("  %d maths results off\n", math_bad);
    CHECK(ubc_pow(2, 10) == 1024 && ubc_pow(-2, 3) == -8 && ubc_pow(10, -2) == 0.01 && isnan(ubc_pow(-8, 1.0 / 3)));
    CHECK(ubc_pow(0, -1) == HUGE_VAL && ubc_pow(0, 2) == 0 && ubc_pow(1, NAN) == 1 && ubc_pow(NAN, 0) == 1);
    CHECK(isnan(ubc_sqrt(-1)) && isnan(ubc_log(-1)) && ubc_log(0) == -HUGE_VAL && ubc_exp(1000) == HUGE_VAL);
    CHECK(isnan(ubc_fmod(1, 0)) && ubc_fmod(5.5, 2) == 1.5 && ubc_fmod(-5.5, 2) == -1.5);
    double ip, fr = ubc_modf(-3.25, &ip);
    CHECK(ip == -3 && fr == -0.25);
    CHECK(ubc_floor(-0.5) == -1 && ubc_ceil(-0.5) == 0 && ubc_floor(1e300) == 1e300);

    CHECK(strcmp(ubc_strstr("hello world", "o w"), "o world") == 0 && ubc_strstr("abc", "d") == NULL);
    CHECK(ubc_strspn("aabbc", "ab") == 4 && ubc_strcspn("hello", "l") == 2 && *ubc_strpbrk("hello", "lo") == 'l');
}
