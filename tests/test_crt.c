/* Uniblox - tests for src/crt/ub_crt.c, compiled under other names so they
 * do not clash with the host libc. Compared against libc for every small
 * length and every source/destination alignment, including overlaps. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define memcpy  ub_test_memcpy
#define memmove ub_test_memmove
#define memset  ub_test_memset
#define memcmp  ub_test_memcmp
#include "../src/crt/ub_crt.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp

#include "test.h"

void test_crt(void)
{
    static unsigned char a[256], b[256], ref[256], buf[256];
    bool ok_cpy = true, ok_set = true, ok_move = true, ok_cmp = true;
    for (int i = 0; i < 256; ++i) a[i] = (unsigned char)(i * 7 + 3);
    for (int so = 0; so < 8; ++so)
        for (int d = 0; d < 8; ++d)
            for (int n = 0; n < 70; ++n) {
                memset(b, 0xAA, sizeof b);
                memset(ref, 0xAA, sizeof ref);
                ub_test_memcpy(b + d, a + so, (size_t)n);
                memcpy(ref + d, a + so, (size_t)n);
                if (memcmp(b, ref, sizeof b)) ok_cpy = false;

                ub_test_memset(b + d, so * 31, (size_t)n);
                memset(ref + d, so * 31, (size_t)n);
                if (memcmp(b, ref, sizeof b)) ok_set = false;

                /* overlapping moves both ways */
                for (int k = 0; k < 256; ++k) buf[k] = ref[k] = (unsigned char)k;
                ub_test_memmove(buf + 20 + d, buf + 20 + so, (size_t)n);
                memmove(ref + 20 + d, ref + 20 + so, (size_t)n);
                if (memcmp(buf, ref, sizeof buf)) ok_move = false;

                int x = ub_test_memcmp(a + so, a + d, (size_t)n), y = memcmp(a + so, a + d, (size_t)n);
                if ((x < 0) != (y < 0) || (x > 0) != (y > 0)) ok_cmp = false;
            }
    CHECK(ok_cpy);
    CHECK(ok_set);
    CHECK(ok_move);
    CHECK(ok_cmp);
}
