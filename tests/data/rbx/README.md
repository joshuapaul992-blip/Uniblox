# Roblox test files

These binary files come from [rojo-rbx/rbx-test-files](https://github.com/rojo-rbx/rbx-test-files)
(MIT License, Copyright (c) 2019-2025 The Rojo Developers). The `.expected`
files are generated from the same project's XML copies of each file by
`tools/gen_rbx_expected.py`, so the binary importer is checked against an
independent source.

| File | Source | Why |
|---|---|---|
| `all-instances-415.rbxl` | `places/all-instances-415` | Every part class, LZ4 chunks |
| `baseplate-727.rbxl` | `places/baseplate-727-with-tags` | Modern Studio file, Zstandard chunks |
| `cframe-special-cases.rbxm` | `models/cframe-special-cases` | The 24 compact rotation IDs |
| `three-unique-parts.rbxm` | `models/three-unique-parts` | Plain parts with distinct properties |
| `weldconstraint.rbxm` | `models/weldconstraint` | Two loose parts joined by a WeldConstraint |
| `modulescript.rbxm` | `models/default-inserted-modulescript` | A script with its code (Source) |

### GUI objects and value objects (milestone 8A)

These `.rbxm` files are the same project's `models/<name>/binary.rbxm`.
`tools/gen_rbx_props_expected.py` copies them and writes each
`<name>.props.expected` from the project's XML copy of the same model: every
property of a class Uniblox keeps, with its value. The importer must
produce all of them.

| File | Why |
|---|---|
| `three-unique-frames.rbxm` | Frames with every GuiObject property set differently |
| `three-screengui.rbxm` | ScreenGui settings |
| `three-uigradients.rbxm` | ColorSequence and NumberSequence values |
| `three-uigridlayouts.rbxm` | Grid layouts (UDim2 cell sizes, enums) |
| `two-imagebuttons.rbxm` | Images, Rect slice centres |
| `text-label-with-font.rbxm`, `font.rbxm` | FontFace (the Font type) |
| `gui-inset-and-font-migration.rbxm` | ScreenGui, TextLabel, TextButton and TextBox together |
| `imagelabel-content.rbxm` | The newer Content type (`ImageContent`) |
| `funny-uipadding.rbxm` | UDim values |
| `three-intvalues.rbxm`, `funny-numbervalue.rbxm`, `three-brickcolorvalues.rbxm`, `three-color3values.rbxm`, `three-vector3values.rbxm` | Value objects (Int64, Float64, BrickColor, Color3, Vector3 with inf and NaN) |

### Attributes and tags (milestone 8B)

The same project's `models/<name>/binary.rbxm`, checked by `test_rbx_attributes` in
`tests/test_world.c`.

| File | Source | Why |
|---|---|---|
| `attributes.rbxm` | `models/attributes` | Every attribute type, with infinity and NaN |
| `tags.rbxm` | `models/tags` | CollectionService tags |
| `font-attribute.rbxm` | `models/folder-with-font-attribute` | A Font attribute |
| `int32-attribute.rbxm` | `models/lighting-with-int32-attribute` | The Int32 attribute type |

### XML twins (milestone 8C)

`xml/<name>.rbxmx` and `xml/<name>.rbxm` are the same project's
`models/<name>/xml.rbxmx` and `binary.rbxm`, and the units `unions.rbxm` and
`unions-sharedstring.rbxm` its `models/unions` and `models/sharedstring`.
`test_rbx_xml` loads each XML file and its binary twin and requires the
same world from both, property by property.
