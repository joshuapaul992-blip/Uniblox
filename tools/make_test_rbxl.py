#!/usr/bin/env python3
"""Write tests/data/rbx/castle-generated.rbxl: a Roblox binary place built
by hand from rbx-dom's format spec, plus its .expected file.

It exercises what the rbx-test-files fixtures do not: rotated parts (raw
CFrame matrices), balls and cylinders, wedges and corner wedges, a truss,
a spawn, a neon part, an invisible part, a MeshPart, Lighting.ClockTime,
and a mix of zstd, lz4 and uncompressed chunks. Needs: zstandard, lz4.
"""
import math, os, struct
import lz4.block, zstandard

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "data", "rbx")

def rf(x):  # Roblox float: IEEE bits rotated left by one, big-endian
    b = struct.unpack(">I", struct.pack(">f", x))[0]
    return ((b << 1) | (b >> 31)) & 0xFFFFFFFF

def interleave(words, width=4):
    raw = b"".join(w.to_bytes(width, "big") for w in words)
    n = len(words)
    return bytes(raw[i * width + b] for b in range(width) for i in range(n))

def transform(i):
    return (i << 1) ^ (i >> 31) & 0xFFFFFFFF if i >= 0 else ((-i) * 2 - 1)

def refs(values):  # accumulated (delta) + transformed + interleaved
    out, prev = [], 0
    for v in values:
        out.append(transform(v - prev) & 0xFFFFFFFF)
        prev = v
    return interleave(out)

def string(s):
    b = s.encode()
    return struct.pack("<I", len(b)) + b

def rot_y(deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return [c, 0, s, 0, 1, 0, -s, 0, c]  # R00..R22, right-handed yaw

class Inst:
    def __init__(s, cls, name, parent, **p):
        s.cls, s.name, s.parent, s.p = cls, name, parent, p

def main():
    items = []
    def add(cls, name, parent=None, **p):
        items.append(Inst(cls, name, parent, **p)); return len(items) - 1
    ws = add("Workspace", "Workspace")
    lt = add("Lighting", "Lighting", clock=17.5)
    castle = add("Model", "Castle", ws)
    grey, dark = 0xA3A2A5, 0x63605F
    add("Part", "Baseplate", ws, pos=(0, -1, 0), size=(160, 2, 160), color=0x4B974B, material=1280)
    add("SpawnLocation", "SpawnLocation", ws, pos=(0, 0.5, 40), size=(8, 1, 8), color=0xC4281C, material=256)
    for i, (x, z) in enumerate([(-20, -20), (20, -20), (-20, 20), (20, 20)]):
        add("Part", "Tower", castle, pos=(x, 9, z), size=(14, 8, 8), shape=2, color=grey, material=816,
            rot=[0, -1, 0, 1, 0, 0, 0, 0, 1])  # cylinder standing up: its X axis turned to world Y
        add("Part", "Roof", castle, pos=(x, 20, z), size=(9, 9, 9), shape=0, color=0x8B2F2F, material=256)
    for i, (x, z, sx, sz) in enumerate([(0, -20, 32, 2), (0, 20, 32, 2), (-20, 0, 2, 32), (20, 0, 2, 32)]):
        add("Part", "Wall", castle, pos=(x, 5, z), size=(sx, 10, sz), color=dark, material=848)
    add("WedgePart", "Ramp", castle, pos=(0, 2, 28), size=(8, 4, 12), color=grey, material=816, rot=rot_y(180))
    add("CornerWedgePart", "Corner", castle, pos=(12, 1.5, 32), size=(4, 3, 4), color=grey, material=816)
    add("TrussPart", "Truss", castle, pos=(-8, 6, 14), size=(2, 12, 2), color=grey, material=1088)
    add("Part", "Crystal", castle, pos=(0, 4, 0), size=(3, 6, 3), color=0x00FFFF, material=288, rot=rot_y(45))
    add("Part", "InvisibleWall", castle, pos=(0, 5, -30), size=(40, 10, 1), color=grey, material=256, transparency=1.0)
    add("MeshPart", "Statue", castle, pos=(8, 3, 0), size=(2, 6, 2), color=0xF5CD30, material=256)

    classes = []
    for it in items:
        if it.cls not in classes: classes.append(it.cls)
    part_classes = {"Part", "WedgePart", "CornerWedgePart", "TrussPart", "SpawnLocation", "MeshPart"}

    chunks = []
    def chunk(name, data, mode):
        if mode == "zstd":
            comp = zstandard.ZstdCompressor(level=5).compress(data)
        elif mode == "lz4":
            comp = lz4.block.compress(data, store_size=False)
        else:
            comp = b""
        chunks.append(name + struct.pack("<III", len(comp), len(data), 0) + (comp if comp else data))

    modes = ["zstd", "lz4", "none"]
    k = 0
    for cid, cls in enumerate(classes):
        members = [i for i, it in enumerate(items) if it.cls == cls]
        service = cls in ("Workspace", "Lighting")
        d = struct.pack("<I", cid) + string(cls) + bytes([1 if service else 0]) + struct.pack("<I", len(members))
        d += refs(members) + (bytes([1] * len(members)) if service else b"")
        chunk(b"INST", d, modes[k % 3]); k += 1
    for cid, cls in enumerate(classes):
        members = [items[i] for i, it in enumerate(items) if it.cls == cls]
        n = len(members)
        def prop(name, type_id, payload):
            nonlocal k
            chunk(b"PROP", struct.pack("<I", cid) + string(name) + bytes([type_id]) + payload, modes[k % 3]); k += 1
        prop("Name", 0x01, b"".join(string(m.name) for m in members))
        if cls == "Lighting":
            prop("ClockTime", 0x04, interleave([rf(m.p["clock"]) for m in members]))
        if cls not in part_classes:
            continue
        cf = b""
        for m in members:
            r = m.p.get("rot")
            cf += b"\x02" if r is None else b"\x00" + struct.pack("<9f", *r)
        cf += b"".join(interleave([rf(m.p["pos"][axis]) for m in members]) for axis in range(3))
        prop("CFrame", 0x10, cf)
        prop("size", 0x0E, b"".join(interleave([rf(m.p["size"][axis]) for m in members]) for axis in range(3)))
        prop("Color3uint8", 0x1A, bytes(m.p["color"] >> 16 for m in members) + bytes((m.p["color"] >> 8) & 255 for m in members)
             + bytes(m.p["color"] & 255 for m in members))
        prop("Material", 0x12, interleave([m.p["material"] for m in members]))
        if cls == "Part":
            prop("shape", 0x12, interleave([m.p.get("shape", 1) for m in members]))
        prop("Transparency", 0x04, interleave([rf(m.p.get("transparency", 0.0)) for m in members]))
        prop("Anchored", 0x02, bytes([1] * n))
        prop("CanCollide", 0x02, bytes([1] * n))
    children = [i for i, it in enumerate(items)]
    parents = [it.parent if it.parent is not None else -1 for it in items]
    chunk(b"PRNT", b"\x00" + struct.pack("<I", len(items)) + refs(children) + refs(parents), "zstd")
    chunks.append(b"END\x00" + struct.pack("<III", 0, 9, 0) + b"</roblox>")

    header = b"<roblox!\x89\xff\x0d\x0a\x1a\x0a" + struct.pack("<HII", 0, len(classes), len(items)) + bytes(8)
    path = os.path.join(OUT, "castle-generated.rbxl")
    with open(path, "wb") as f:
        f.write(header + b"".join(chunks))
    lines = []
    for it in items:
        if it.cls not in part_classes: continue
        p = it.p
        r = p.get("rot", [1, 0, 0, 0, 1, 0, 0, 0, 1])
        lines.append("%s|%s|%.6g %.6g %.6g|%.6g %.6g %.6g|%d|%d|%d|%s|%.6g" % (
            it.cls, it.name, *p["pos"], *p["size"], p["color"], p["material"], p.get("shape", 1),
            " ".join("%.6g" % v for v in r), p.get("transparency", 0.0)))
    with open(os.path.join(OUT, "castle-generated.expected"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path, os.path.getsize(path), "bytes,", len(items), "instances")

if __name__ == "__main__":
    main()
