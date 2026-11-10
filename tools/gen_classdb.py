#!/usr/bin/env python3
"""Write src/world/ub_classdb.inc: the classes, properties, defaults and
enums Uniblox knows, from rbx-dom's reflection database.

    tools/gen_classdb.py /path/to/rbx-dom/rbx_dom_lua/src/database.json

rbx-dom (https://github.com/rojo-rbx/rbx-dom, MIT licence) records Roblox's
public API: every class, its properties and their types, the values a new
instance has, and every enum. Only facts about the API are taken (names,
types, numbers); nothing else.

Kept: the classes in CLASSES and their superclasses; per class, the
properties it declares that scripts can read and that have a type Uniblox
stores; per creatable class, the defaults of all its stored properties;
the enums those properties use plus EXTRA_ENUMS.
"""
import json
import math
import os
import sys

CLASSES = [
    # GUI
    "Frame", "ScrollingFrame", "TextLabel", "TextButton", "TextBox", "ImageLabel", "ImageButton",
    "ViewportFrame", "VideoFrame", "CanvasGroup", "ScreenGui", "BillboardGui", "SurfaceGui",
    "UIAspectRatioConstraint", "UISizeConstraint", "UITextSizeConstraint", "UIListLayout", "UIGridLayout",
    "UIPageLayout", "UITableLayout", "UICorner", "UIGradient", "UIPadding", "UIScale", "UIStroke",
    "PlayerGui", "StarterGui",
    # values and events
    "IntValue", "NumberValue", "StringValue", "BoolValue", "ObjectValue", "Color3Value", "Vector3Value",
    "BrickColorValue", "CFrameValue", "RayValue",
    "RemoteEvent", "RemoteFunction", "BindableEvent", "BindableFunction", "Folder", "Configuration",
    # milestone 8B: what 2022 games' scripts read and set
    "UnreliableRemoteEvent", "Sound", "SoundGroup", "Attachment", "ClickDetector", "ProximityPrompt", "Highlight",
    "Decal", "Texture", "PointLight", "SpotLight", "SurfaceLight", "ParticleEmitter", "Fire", "Smoke", "Sparkles",
    "Trail", "Beam", "Tool", "Team", "Sky", "Atmosphere", "Clouds", "BloomEffect", "BlurEffect",
    "ColorCorrectionEffect", "SunRaysEffect", "DepthOfFieldEffect", "Animation", "SelectionBox", "Dialog",
    "SpecialMesh", "BlockMesh", "CylinderMesh", "Explosion", "ForceField", "Accessory", "Hat", "Shirt", "Pants",
    "ShirtGraphic", "BodyColors", "HumanoidDescription", "StarterGear",
]

EXTRA_ENUMS = [
    "KeyCode", "UserInputType", "UserInputState", "MouseBehavior", "CoreGuiType", "NormalId", "FontWeight",
    "FontStyle", "Font", "ThumbnailType", "ThumbnailSize", "EasingStyle", "EasingDirection", "PlaybackState",
    "TextInputType", "SelectionBehavior", "CameraType", "ModifierKey", "ScreenOrientation", "FillDirection",
    "HorizontalAlignment", "VerticalAlignment", "SortOrder", "FontSize", "TweenStatus", "Platform",
    "HumanoidStateType", "HumanoidRigType", "ProximityPromptStyle", "ProximityPromptInputType", "Material",
    "PartType", "SurfaceType", "AnimationPriority", "RaycastFilterType", "PathStatus", "Limb", "AssetType", "Axis", "PathWaypointAction", "ProximityPromptExclusivity",
]

# properties never stored as data (Uniblox handles them itself or they mean nothing here)
SKIP_PROPS = {"Name", "Parent", "Archivable", "Attributes", "Tags", "Capabilities", "Sandboxed", "HistoryId",
              "UniqueId", "SourceAssetId", "SerializedOverrides", "DefinesCapabilities", "ClassName",
              "RobloxLocked", "ImageContent", "HoverImageContent", "PressedImageContent", "VideoContent"}

TYPES = {
    "Bool": "UB_PV_BOOL", "Int32": "UB_PV_INT", "Int64": "UB_PV_DOUBLE", "Float32": "UB_PV_FLOAT",
    "Float64": "UB_PV_DOUBLE", "String": "UB_PV_STRING", "ContentId": "UB_PV_STRING", "Color3": "UB_PV_COLOR3",
    "Vector2": "UB_PV_VECTOR2", "Vector3": "UB_PV_VECTOR3", "UDim": "UB_PV_UDIM", "UDim2": "UB_PV_UDIM2",
    "Rect": "UB_PV_RECT", "NumberRange": "UB_PV_NUMRANGE", "NumberSequence": "UB_PV_NUMSEQ",
    "ColorSequence": "UB_PV_COLORSEQ", "Ref": "UB_PV_REF", "Font": "UB_PV_FONT", "BrickColor": "UB_PV_BRICKCOLOR",
}

WEIGHTS = {"Thin": 100, "ExtraLight": 200, "Light": 300, "Regular": 400, "Medium": 500, "SemiBold": 600,
           "Bold": 700, "ExtraBold": 800, "Heavy": 900}


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cf(v):
    if v is None:
        return "UB_CDB_INF"
    if isinstance(v, bool):
        return "1" if v else "0"
    v = float(v)
    if math.isinf(v):
        return "UB_CDB_INF" if v > 0 else "-UB_CDB_INF"
    r = repr(v)
    if "e" not in r and "." not in r:
        r += ".0"
    return r + "f"


def ptype(p):
    dt = p["DataType"]
    if "Enum" in dt:
        return "UB_PV_ENUM", dt["Enum"]
    return TYPES.get(dt.get("Value")), None


def default_values(t, v):
    """(n0..n3, string) for a default value as rbx-dom writes it."""
    (kind, val), = v.items()
    if kind == "Bool":
        return [1 if val else 0, 0, 0, 0], None
    if kind in ("Int32", "Int64", "Float32", "Float64", "Enum", "BrickColor"):
        return [val, 0, 0, 0], None
    if kind in ("String", "ContentId", "Content"):
        if isinstance(val, dict):
            val = ""
        return [0, 0, 0, 0], val if isinstance(val, str) else ""
    if kind in ("Color3", "Vector3"):
        return list(val) + [0], None
    if kind == "Vector2":
        return list(val) + [0, 0], None
    if kind == "UDim":
        return [val[0], val[1], 0, 0], None
    if kind == "UDim2":
        return [val[0][0], val[0][1], val[1][0], val[1][1]], None
    if kind == "Rect":
        return [val[0][0], val[0][1], val[1][0], val[1][1]], None
    if kind == "NumberRange":
        return [val[0], val[1], 0, 0], None
    if kind == "NumberSequence":
        k = val["keypoints"]
        assert len(k) == 2 and k[0]["value"] == k[1]["value"], k
        return [k[0]["value"], 0, 0, 0], None
    if kind == "ColorSequence":
        k = val["keypoints"]
        assert len(k) == 2 and k[0]["color"] == k[1]["color"], k
        return list(k[0]["color"]) + [0], None
    if kind == "Ref":
        return [-1, 0, 0, 0], None
    if kind == "Font":
        return [WEIGHTS.get(val["weight"], 400), 1 if val["style"] == "Italic" else 0, 0, 0], val["family"]
    raise ValueError(kind)


def main(path):
    db = json.load(open(path, encoding="utf-8"))
    classes = db["Classes"]
    enums = db["Enums"]

    order = []
    def add(name):
        if name in order:
            return
        sup = classes[name]["Superclass"]
        if sup:
            add(sup)
        order.append(name)
    for c in CLASSES:
        add(c)

    props = []           # (class index, name, type, enum, flags)
    class_rows = []
    used_enums = []
    def use_enum(e):
        if e not in used_enums:
            used_enums.append(e)

    for ci, name in enumerate(order):
        c = classes[name]
        first = len(props)
        for pn in sorted(c["Properties"]):
            p = c["Properties"][pn]
            if pn in SKIP_PROPS or "NotScriptable" in p["Tags"] or p["Scriptability"] not in ("ReadWrite", "Read"):
                continue
            if "Alias" in p["Kind"]:
                continue
            t, en = ptype(p)
            if not t:
                continue
            ser = p["Kind"]["Canonical"]["Serialization"]
            readonly = p["Scriptability"] == "Read"
            stored = not readonly and (ser == "Serializes" or isinstance(ser, dict) or pn == "Font")
            flags = []
            if readonly:
                flags.append("UB_CDB_READONLY")
            if stored:
                flags.append("UB_CDB_STORED")
            if "Deprecated" in p["Tags"]:
                flags.append("UB_CDB_DEPRECATED")
            if en:
                use_enum(en)
            props.append((ci, pn, t, en, "|".join(flags) or "0"))
        class_rows.append([name, order.index(c["Superclass"]) if c["Superclass"] else -1, first, len(props) - first])

    def find_prop(ci, pn):
        while ci >= 0:
            for i, p in enumerate(props):
                if p[0] == ci and p[1] == pn:
                    return i
            ci = class_rows[ci][1]
        return -1

    defs = []
    for ci, name in enumerate(order):
        c = classes[name]
        dp = c.get("DefaultProperties", {})
        first = len(defs)
        if dp:
            # every stored property of the class and its superclasses
            chain = []
            k = ci
            while k >= 0:
                chain.append(k)
                k = class_rows[k][1]
            for i, p in enumerate(props):
                if p[0] not in chain or "UB_CDB_STORED" not in p[4]:
                    continue
                if p[1] == "Font" and p[2] == "UB_PV_ENUM":
                    nums, s = [0, 0, 0, 0], None  # Legacy, as a new label had in 2022
                elif p[1] in dp:
                    nums, s = default_values(p[2], dp[p[1]])
                elif p[2] == "UB_PV_REF":
                    nums, s = [-1, 0, 0, 0], None  # nil (rbx-dom leaves nil references out)
                else:
                    continue
                defs.append((i, nums, s))
        class_rows[ci] += [first, len(defs) - first]

    for e in EXTRA_ENUMS:
        if e in enums:
            use_enum(e)
    enum_rows, items = [], []
    for e in used_enums:
        first = len(items)
        for iname, val in sorted(enums[e]["items"].items(), key=lambda kv: (kv[1], kv[0])):
            items.append((iname, val))
        enum_rows.append((e, first, len(items) - first))
    enum_index = {e: i for i, e in enumerate(used_enums)}

    out = []
    w = out.append
    w("/* Generated by tools/gen_classdb.py from rbx-dom's reflection database")
    w(" * (database version %s). Do not edit. */" % ".".join(str(x) for x in db["Version"]))
    w("")
    w("static const cdb_class k_cdb_classes[] = {")
    for r in class_rows:
        w("    { %s, %d, %d, %d, %d, %d }," % (cstr(r[0]), r[1], r[2], r[3], r[4], r[5]))
    w("};")
    w("")
    w("static const ub_cdb_prop k_cdb_props[] = {")
    for ci, pn, t, en, flags in props:
        w("    { %s, %s, %d, %s }," % (cstr(pn), t, enum_index[en] if en else -1, flags))
    w("};")
    w("")
    w("static const cdb_default k_cdb_defaults[] = {")
    for i, nums, s in defs:
        w("    { %d, { %s }, %s }," % (i, ", ".join(cf(x) for x in nums), cstr(s) if s is not None else "NULL"))
    w("};")
    w("")
    w("static const cdb_enum k_cdb_enums[] = {")
    for e, first, n in enum_rows:
        w("    { %s, %d, %d }," % (cstr(e), first, n))
    w("};")
    w("")
    w("static const cdb_item k_cdb_items[] = {")
    for iname, val in items:
        w("    { %s, %d }," % (cstr(iname), val))
    w("};")
    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "world", "ub_classdb.inc")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("%d classes, %d properties, %d defaults, %d enums (%d items)" %
          (len(class_rows), len(props), len(defs), len(enum_rows), len(items)))


if __name__ == "__main__":
    main(sys.argv[1])
