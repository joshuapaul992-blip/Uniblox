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
