#!/usr/bin/env python3
"""Copy rbx-test-files' GUI and value-object models into tests/data/rbx and
write <name>.props.expected from each one's XML copy.

  tools/gen_rbx_props_expected.py /path/to/rbx-test-files

One line per property the importer must keep (classes in Uniblox's class
table, types it stores), read from the XML, so the binary importer is
checked against an independent source:
  path|property|type|value...
path is the instance's names from the root, joined by '/'. Values are
numbers separated by spaces; strings are written as-is after the type.
"""
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MODELS = ["three-unique-frames", "three-screengui", "three-uigradients", "three-uigridlayouts",
          "two-imagebuttons", "text-label-with-font", "gui-inset-and-font-migration", "imagelabel-content",
          "funny-uipadding", "three-intvalues", "three-brickcolorvalues", "funny-numbervalue",
          "three-color3values", "three-vector3values", "font"]

SKIP = {"Name", "AttributesSerialize", "Tags", "SourceAssetId", "UniqueId", "HistoryId", "Capabilities",
        "DefinesCapabilities", "Sandboxed", "SerializedOverrides", "Archivable"}
STYLE = {"Normal": 0, "Italic": 1}


def kept_classes():
    """The classes in src/world/ub_classdb.inc (the importer keeps their properties)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "world", "ub_classdb.inc")
    text = open(path, encoding="utf-8").read()
    block = text.split("k_cdb_classes[] = {")[1].split("};")[0]
    return set(re.findall(r'\{ "(\w+)"', block))


def f(x):
    return repr(float(x))


def value(el, by_ref):
    """(type, value text) for a property element, or None if not kept."""
    t, txt = el.tag, (el.text or "").strip()
    child = lambda n: el.find(n).text.strip()
    if t == "bool":
        return "bool", "1" if txt == "true" else "0"
    if t == "int":
        return "int", str(int(txt))
    if t in ("int64", "double"):
        return "double", f(txt)
    if t == "float":
        return "float", f(float(txt.replace("INF", "inf")))
    if t == "token":
        return "enum", str(int(txt))
    if t in ("string", "ProtectedString"):
        return "string", el.text or ""
    if t == "Content":
        u = el.find("url")
        if u is None:
            u = el.find("uri")
        return ("string", u.text or "") if u is not None else None
    if t == "BrickColor":
        return "brickcolor", str(int(txt))
    if t == "Color3":
        return "color3", " ".join(f(child(c)) for c in "RGB")
    if t == "Color3uint8":
        v = int(txt) & 0xFFFFFF
        return "color3", " ".join(f(((v >> s) & 255) / 255.0) for s in (16, 8, 0))
    if t == "Vector2":
        return "vector2", " ".join(f(child(c)) for c in "XY")
    if t == "Vector3":
        return "vector3", " ".join(f(child(c)) for c in "XYZ")
    if t == "UDim":
        return "udim", "%s %d" % (f(child("S")), int(child("O")))
    if t == "UDim2":
        return "udim2", "%s %d %s %d" % (f(child("XS")), int(child("XO")), f(child("YS")), int(child("YO")))
    if t == "Rect2D":
        mn, mx = el.find("min"), el.find("max")
        return "rect", " ".join(f(e.find(c).text) for e in (mn, mx) for c in "XY")
    if t == "NumberRange":
        return "numrange", " ".join(f(x) for x in txt.split()[:2])
    if t == "NumberSequence":
        return "numseq", " ".join(f(x) for x in txt.split())
    if t == "ColorSequence":
        nums = txt.split()
        out = []
        for i in range(0, len(nums), 5):
            out += nums[i:i + 4]  # time r g b (the envelope is unused)
        return "colorseq", " ".join(f(x) for x in out)
    if t == "Ref":
        return "ref", by_ref.get(txt, "")
    if t == "Font":
        fam = el.find("Family").find("url").text or ""
        return "font", "%d %d %s" % (int(el.find("Weight").text), STYLE.get(el.find("Style").text, 0), fam)
    return None


def main(root):
    keep = kept_classes()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "data", "rbx")
    for name in MODELS:
        src = os.path.join(root, "models", name)
        shutil.copyfile(os.path.join(src, "binary.rbxm"), os.path.join(out_dir, name + ".rbxm"))
        tree = ET.parse(os.path.join(src, "xml.rbxmx"))
        by_ref, items = {}, []

        def walk(item, path):
            props = item.find("Properties")
            nm = props.find("string[@name='Name']")
            p = path + [nm.text if nm is not None and nm.text else item.get("class")]
            by_ref[item.get("referent")] = "/".join(p)
            items.append((item, "/".join(p)))
            for c in item.findall("Item"):
                walk(c, p)
        for item in tree.getroot().findall("Item"):
            walk(item, [])
        lines = []
        for item, path in items:
            if item.get("class") not in keep:
                continue
            for el in item.find("Properties"):
                pname = el.get("name")
                if pname in SKIP:
                    continue
                if el.tag == "Content" and pname.endswith("Content"):
                    pname = pname[:-7]
                v = value(el, by_ref)
                if v is None:
                    continue
                lines.append("%s|%s|%s|%s" % (path, pname, v[0], v[1]))
        with open(os.path.join(out_dir, name + ".props.expected"), "w", encoding="utf-8", newline="\n") as fo:
            fo.write("\n".join(lines) + "\n")
        print("%s: %d properties" % (name, len(lines)))


if __name__ == "__main__":
    main(sys.argv[1])
