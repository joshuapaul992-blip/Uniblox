#!/usr/bin/env python3
"""Generate LZ4/Zstandard test vectors for tests/test_compress.c.

Needs: pip install zstandard lz4. Writes tests/data/*.zst, *.lz4 and
tests/data/compress_manifest.txt with lines "<file> <size> <fnv1a64-hex>"
describing the expected decompressed output. Deterministic (fixed seeds).
"""
import os, random, struct, sys
import zstandard, lz4.block

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
    out = []
    size = 0
    while size < n:
        w = r.choice(vocab) + r.choice([" ", " ", " ", ", ", ".\n"])
        out.append(w)
        size += len(w)
    return "".join(out).encode()[:n]

def structured(n, seed):
    """Interleaved floats and ints, like Roblox PROP chunks."""
    r = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        # mostly repeated values, as in real part properties
        x = r.uniform(-500, 500) if r.random() < 0.03 else r.choice([0.0, 1.0, 2.5, 4.0])
        k = r.randint(0, 50) if r.random() < 0.03 else 7
        out += struct.pack(">f", x) + bytes([k & 3, 0, 0, 1]) + b"Part" + struct.pack("<I", k)
    return bytes(out[:n])

def main():
    os.makedirs(OUT, exist_ok=True)
    r = random.Random(7)
    inputs = {
        "text32k": text(32 * 1024, 1),
        "struct300k": structured(300 * 1024, 2),   # several 128 KB blocks
        "random4k": bytes(r.getrandbits(8) for _ in range(4096)),
        "zeros256k": bytes(256 * 1024),
        "tiny": b"a",
        "short": b"Hello, Uniblox! Hello, Uniblox! Hello!",
        "mixed": text(12000, 3) + bytes(5000) + structured(30000, 4) + bytes(r.getrandbits(8) for _ in range(1500)),
    }
    lines = []
    def emit(name, blob, data):
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(blob)
        lines.append("%s %d %016x" % (name, len(data), fnv1a64(data)))
    for key, data in inputs.items():
        levels = [1, 19, -5] if key == "mixed" else [1, 19] if key == "text32k" else [3]
        if key == "struct300k":
            levels = [3, 19]
        for lvl in levels:
            c = zstandard.ZstdCompressor(level=lvl, write_checksum=(lvl == 19)).compress(data)
            emit("%s.l%d.zst" % (key, lvl), c, data)
        emit("%s.lz4" % key, lz4.block.compress(data, store_size=False, mode="high_compression"), data)
    with open(os.path.join(OUT, "compress_manifest.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    total = sum(os.path.getsize(os.path.join(OUT, l.split()[0])) for l in lines)
    print("%d vectors, %d bytes" % (len(lines), total))

if __name__ == "__main__":
    main()
