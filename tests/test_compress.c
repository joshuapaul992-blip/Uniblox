/* Uniblox - tests for the LZ4 and Zstandard decoders.
 *
 * Vectors in tests/data come from tools/gen_compress_vectors.py (Python's
 * zstandard and lz4 modules): each must decode to exactly the size and
 * FNV-1a hash in the manifest. Then every vector is corrupted thousands of
 * ways (bit flips, truncation, garbage) under ASan/UBSan: decoding may fail,
 * but must never read or write out of bounds. */
#include <stdint.h>
#include <stdlib.h>

#include "core/ub_compress.h"
#include "test.h"

static u8 *read_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (u32)n;
    return buf;
}

static u64 fnv1a64(const u8 *p, u32 n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static i32 decode(const char *name, const u8 *src, u32 n, u8 *dst, u32 cap)
{
    size_t l = strlen(name);
    if (l > 4 && strcmp(name + l - 4, ".lz4") == 0) return ub_lz4_decompress(src, n, dst, cap);
    return ub_zstd_decompress(src, n, dst, cap);
}

void test_compress(void)
{
    FILE *m = fopen("tests/data/compress_manifest.txt", "r");
    CHECK(m != NULL);
    if (!m) return;
    char name[128];
    unsigned long size;
    unsigned long long hash;
    int vectors = 0, ok = 0;
    u32 seed = 12345;
    while (fscanf(m, "%127s %lu %llx", name, &size, &hash) == 3) {
        char path[256];
        snprintf(path, sizeof path, "tests/data/%s", name);
        u32 n;
        u8 *src = read_file(path, &n);
        CHECK(src != NULL);
        if (!src) continue;
        vectors++;
        u8 *dst = (u8 *)malloc(size + 1);
        i32 got = decode(name, src, n, dst, (u32)size);
        bool good = got == (i32)size && fnv1a64(dst, (u32)size) == hash;
        if (!good) printf("    %s: got %d bytes (want %lu)\n", name, got, size);
        ok += good;

        /* one byte too little room must fail cleanly */
        if (size > 0) CHECK(decode(name, src, n, dst, (u32)size - 1) == -1);

        /* corruption: must not crash or overrun (ASan watches) */
        u8 *bad = (u8 *)malloc(n);
        for (int t = 0; t < 300; ++t) {
            memcpy(bad, src, n);
            int flips = 1 + t % 4;
            for (int k = 0; k < flips; ++k) {
                seed = seed * 1103515245u + 12345u;
                u32 at = (seed >> 8) % n;
                bad[at] ^= (u8)(1u << ((seed >> 4) & 7));
            }
            seed = seed * 1103515245u + 12345u;
            u32 len = (t & 1) ? n : (seed >> 8) % (n + 1); /* also truncate */
            decode(name, bad, len, dst, (u32)size);
        }
        free(bad);
        free(dst);
        free(src);
    }
    fclose(m);
    printf("  %d/%d vectors decode exactly\n", ok, vectors);
    CHECK(vectors >= 18 && ok == vectors);

    /* tiny hand-made cases */
    u8 out[64];
    static const u8 lz4_lit[] = { 0x50, 'h', 'e', 'l', 'l', 'o' };
    CHECK(ub_lz4_decompress(lz4_lit, sizeof lz4_lit, out, 5) == 5 && memcmp(out, "hello", 5) == 0);
    static const u8 lz4_bad_offset[] = { 0x10, 'a', 0x05, 0x00 }; /* offset beyond output */
    CHECK(ub_lz4_decompress(lz4_bad_offset, sizeof lz4_bad_offset, out, 64) == -1);
    CHECK(!ub_zstd_is_frame((const u8 *)"abcd", 4) && ub_zstd_is_frame((const u8 *)"\x28\xb5\x2f\xfd", 4));
    CHECK(ub_zstd_decompress((const u8 *)"\x28\xb5\x2f\xfd", 4, out, 64) == -1);

    /* random garbage after a valid magic */
    for (int t = 0; t < 2000; ++t) {
        u8 junk[200];
        junk[0] = 0x28; junk[1] = 0xb5; junk[2] = 0x2f; junk[3] = 0xfd;
        for (int i = 4; i < 200; ++i) {
            seed = seed * 1103515245u + 12345u;
            junk[i] = (u8)(seed >> 16);
        }
        ub_zstd_decompress(junk, 4 + (u32)(t % 196), out, sizeof out);
        ub_lz4_decompress(junk + 4, (u32)(t % 196), out, sizeof out);
    }
}
