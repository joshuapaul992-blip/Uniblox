/* Uniblox - decompressors for LZ4 blocks, Zstandard frames and deflate.
 *
 * Both decode into a caller-supplied buffer whose size is known up front
 * (Roblox binary place files store each chunk's uncompressed length), and
 * treat their input as untrusted: every read and write is bounds-checked.
 * No dictionaries; Zstandard checksums are skipped, not verified. */
#ifndef UB_COMPRESS_H
#define UB_COMPRESS_H

#include "core/ub_types.h"

/* LZ4 block format (no frame). Returns bytes written, or -1 if the input
 * is malformed or would not fit in dst_cap. */
i32 ub_lz4_decompress(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap);

/* One or more Zstandard frames (skippable frames are skipped). Returns
 * bytes written, or -1 on malformed input / not enough room. */
i32 ub_zstd_decompress(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap);

/* True if the data starts with the Zstandard frame magic 28 B5 2F FD. */
bool ub_zstd_is_frame(const u8 *src, u32 len);

/* Deflate (RFC 1951) inside a zlib wrapper (RFC 1950), as PNG images use.
 * The Adler-32 checksum is verified. Returns bytes written, or -1 on
 * malformed input / not enough room. */
i32 ub_zlib_decompress(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap);
/* Raw deflate data, no wrapper; *used gets the input bytes consumed. */
i32 ub_inflate(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap, u32 *used);

#endif
