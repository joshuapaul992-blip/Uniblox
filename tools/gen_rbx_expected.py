#!/usr/bin/env python3
"""Write tests/data/rbx/<name>.expected from rbx-test-files' XML copies.

  tools/gen_rbx_expected.py /path/to/rbx-test-files

One line per part-like instance, in Roblox's own coordinates and units:
  class|name|px py pz|sx sy sz|color|material|shape|R00..R22|transparency
"""
import os, re, sys

PART_CLASSES = {"Part", "WedgePart", "CornerWedgePart", "TrussPart", "SpawnLocation", "Seat",
                "VehicleSeat", "MeshPart", "UnionOperation"}
FILES = {
    "all-instances-415": "places/all-instances-415/xml.rbxlx",
    "baseplate-727": "places/baseplate-727-with-tags/xml.rbxlx",
    "three-unique-parts": "models/three-unique-parts/xml.rbxmx",
}

def num(block, tag):
    m = re.search(r"<%s>([^<]*)</%s>" % (tag, tag), block)
    return float(m.group(1)) if m else 0.0

def main(root):
    out_dir = os.path.join(os.path.dirname(__file__), "..", "tests", "data", "rbx")
    for name, rel in FILES.items():
        xml = open(os.path.join(root, rel), encoding="utf-8").read()
        lines = []
        # each <Item ...><Properties>...</Properties> (children follow, so cut at </Properties>)
        for m in re.finditer(r'<Item class="(\w+)"[^>]*>\s*<Properties>(.*?)</Properties>', xml, re.S):
            cls, props = m.group(1), m.group(2)
            if cls not in PART_CLASSES:
                continue
            cf = re.search(r'<CoordinateFrame name="CFrame">(.*?)</CoordinateFrame>', props, re.S).group(1)
            size = re.search(r'<Vector3 name="size">(.*?)</Vector3>', props, re.S)
            size = size.group(1) if size else re.search(r'<Vector3 name="Size">(.*?)</Vector3>', props, re.S).group(1)
            nm = re.search(r'<string name="Name">(.*?)</string>', props, re.S).group(1)
            col = re.search(r'<Color3uint8 name="Color3uint8">(\d+)</Color3uint8>', props)
            color = (int(col.group(1)) & 0xFFFFFF) if col else -1
            mat = re.search(r'<token name="Material">(\d+)</token>', props)
            shp = re.search(r'<token name="shape">(\d+)</token>', props)
            tr = re.search(r'<float name="Transparency">([^<]*)</float>', props)
            rot = " ".join("%.6g" % num(cf, "R%d%d" % (i, j)) for i in range(3) for j in range(3))
            lines.append("%s|%s|%.6g %.6g %.6g|%.6g %.6g %.6g|%d|%d|%d|%s|%.6g" % (
                cls, nm, num(cf, "X"), num(cf, "Y"), num(cf, "Z"),
                num(size, "X"), num(size, "Y"), num(size, "Z"), color,
                int(mat.group(1)) if mat else 256, int(shp.group(1)) if shp else 1, rot,
                float(tr.group(1)) if tr else 0.0))
        with open(os.path.join(out_dir, name + ".expected"), "w") as f:
            f.write("\n".join(lines) + "\n")
        print(name, len(lines), "parts")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "../ref/rbx-test-files")
