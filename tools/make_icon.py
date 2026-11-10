#!/usr/bin/env python3
"""Uniblox - draw the program icons (.ico) at build time.

    tools/make_icon.py game|editor out.ico

Each icon holds 32x32 and 16x16 images in 16 colours (all NT 3.51 and 4.0
can show) and in 32-bit colour with a soft edge (2000 and later pick
those). The game's is a red block seen from above a corner; the editor's
is a blue block with a pencil.
"""
import struct
import sys

VGA16 = [
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255),
    (255, 255, 255),
]


def inside(poly, x, y):
    """Point in convex polygon (any winding)."""
    sign = 0
    n = len(poly)
    for i in range(n):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % n]
        c = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
        if c != 0:
            s = 1 if c > 0 else -1
            if sign == 0:
                sign = s
            elif s != sign:
                return False
    return True


def draw(kind, size):
    """RGBA pixels, rows top-down, with 4x4 supersampling."""
    s = size / 32.0
    # a block: top, left and right faces of an isometric cube
    cx, top, mid, bot, half = 16 * s, 3 * s, 10.5 * s, 29 * s, 13 * s
    faces_game = [
        ([(cx, top), (cx + half, top + 7.5 * s), (cx, mid + 7.5 * s), (cx - half, top + 7.5 * s)], (255, 96, 80)),
        ([(cx - half, top + 7.5 * s), (cx, mid + 7.5 * s), (cx, bot), (cx - half, bot - 7.5 * s)], (200, 40, 28)),
        ([(cx + half, top + 7.5 * s), (cx + half, bot - 7.5 * s), (cx, bot), (cx, mid + 7.5 * s)], (130, 20, 16)),
    ]
    faces = faces_game
    if kind == "editor":
        faces = [(p, c) for p, c in zip([f[0] for f in faces_game], [(120, 190, 255), (40, 110, 220), (20, 60, 150)])]
        # a pencil across the front corner
        p0, p1, w = (9 * s, 30 * s), (29 * s, 10 * s), 2.6 * s
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        ln = (dx * dx + dy * dy) ** 0.5
        nx, ny = -dy / ln * w, dx / ln * w
        tip = (p0[0] - dx / ln * 4 * s, p0[1] - dy / ln * 4 * s)
        faces.append(([(p0[0] + nx, p0[1] + ny), (p1[0] + nx, p1[1] + ny), (p1[0] - nx, p1[1] - ny),
                       (p0[0] - nx, p0[1] - ny)], (255, 210, 40)))
        faces.append(([(p0[0] + nx, p0[1] + ny), (p0[0] - nx, p0[1] - ny), tip], (60, 40, 30)))
    px = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    fx, fy = x + (sx + 0.5) / 4, y + (sy + 0.5) / 4
                    col = None
                    for poly, c in faces:  # later faces on top
                        if inside(poly, fx, fy):
                            col = c
                    if col:
                        acc[0] += col[0]
                        acc[1] += col[1]
                        acc[2] += col[2]
                        acc[3] += 1
            if acc[3]:
                row.append((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] * 255 // 16))
            else:
                row.append((0, 0, 0, 0))
        px.append(row)
    return px


def nearest16(c):
    return min(range(16), key=lambda i: sum((a - b) ** 2 for a, b in zip(VGA16[i], c)))


def image(px, bpp):
    size = len(px)
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, bpp, 0, 0, 0, 0, 16 if bpp == 4 else 0, 0)
    data = bytearray(hdr)
    if bpp == 4:
        for r, g, b in VGA16:
            data += bytes((b, g, r, 0))
    rows = list(reversed(px))  # bottom-up
    for row in rows:
        if bpp == 4:
            line = bytearray()
            for x in range(0, size, 2):
                a = nearest16(row[x][:3]) if row[x][3] >= 128 else 0
                b = nearest16(row[x + 1][:3]) if row[x + 1][3] >= 128 else 0
                line.append((a << 4) | b)
            while len(line) % 4:
                line.append(0)
            data += line
        else:
            for r, g, b, a in row:
                data += bytes((b, g, r, a))
    for row in rows:  # AND mask: 1 = transparent
        line = bytearray((size + 31) // 32 * 4)
        for x in range(size):
            if row[x][3] < 128:
                line[x // 8] |= 0x80 >> (x % 8)
        data += line
    return bytes(data)


def main():
    kind, out = sys.argv[1], sys.argv[2]
    imgs = []
    for size in (32, 16):
        px = draw(kind, size)
        imgs.append((size, 4, image(px, 4)))
        imgs.append((size, 32, image(px, 32)))
    head = struct.pack("<HHH", 0, 1, len(imgs))
    offset = 6 + 16 * len(imgs)
    entries, blobs = b"", b""
    for size, bpp, blob in imgs:
        entries += struct.pack("<BBBBHHII", size, size, 16 if bpp == 4 else 0, 0, 1, bpp, len(blob), offset)
        offset += len(blob)
        blobs += blob
    with open(out, "wb") as f:
        f.write(head + entries + blobs)


if __name__ == "__main__":
    main()
