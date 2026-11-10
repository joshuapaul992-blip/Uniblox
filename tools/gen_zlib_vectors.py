#!/usr/bin/env python3
"""Generate zlib (deflate) test vectors for tests/test_compress.c.

Uses only Python's own zlib. Writes tests/data/*.zlib and appends their
lines ("<file> <size> <fnv1a64-hex>") to tests/data/compress_manifest.txt.
The inputs are those of gen_compress_vectors.py (same seeds), coded with
stored, fixed-Huffman, Huffman-only, RLE and dynamic blocks.
"""
import os, random, struct, zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "data")

def fnv1a64(b):
    h = 0xcbf29ce484222325
    for c in b:
        h ^= c
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h

def text(n, seed):
    r = random.Random(seed)
    vocab = ["".join(r.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(2, 9))) for _ in range(3000)]
    out, size = [], 0
    while size < n:
        w = r.choice(vocab) + r.choice([" ", " ", " ", ", ", ".\n"])
        out.append(w)
        size += len(w)
    return "".join(out).encode()[:n]

def structured(n, seed):
    r = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        x = r.uniform(-500, 500) if r.random() < 0.03 else r.choice([0.0, 1.0, 2.5, 4.0])
        k = r.randint(0, 50) if r.random() < 0.03 else 7
        out += struct.pack(">f", x) + bytes([k & 3, 0, 0, 1]) + b"Part" + struct.pack("<I", k)
    return bytes(out[:n])

def deflate(data, level, strategy):
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    return c.compress(data) + c.flush()

def main():
    r = random.Random(7)
    inputs = {
        "text32k": text(32 * 1024, 1),
        "struct300k": structured(300 * 1024, 2),
        "random4k": bytes(r.getrandbits(8) for _ in range(4096)),
        "tiny": b"a",
    }
    plans = {
        "text32k": [("l0", 0, zlib.Z_DEFAULT_STRATEGY), ("l9", 9, zlib.Z_DEFAULT_STRATEGY),
                    ("fixed", 6, zlib.Z_FIXED), ("huff", 6, zlib.Z_HUFFMAN_ONLY)],
        "struct300k": [("l1", 1, zlib.Z_DEFAULT_STRATEGY), ("rle", 6, zlib.Z_RLE)],
        "random4k": [("l6", 6, zlib.Z_DEFAULT_STRATEGY)],
        "tiny": [("l6", 6, zlib.Z_DEFAULT_STRATEGY)],
    }
    lines = []
    for key, data in inputs.items():
        for tag, level, strategy in plans[key]:
            name = "%s.%s.zlib" % (key, tag)
            with open(os.path.join(OUT, name), "wb") as f:
                f.write(deflate(data, level, strategy))
            lines.append("%s %d %016x" % (name, len(data), fnv1a64(data)))
    manifest = os.path.join(OUT, "compress_manifest.txt")
    old = [l for l in open(manifest).read().splitlines() if not l.split(" ")[0].endswith(".zlib")]
    with open(manifest, "w", newline="\n") as f:
        f.write("\n".join(old + lines) + "\n")
    print("%d zlib vectors" % len(lines))

if __name__ == "__main__":
    main()
