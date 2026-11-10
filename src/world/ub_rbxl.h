/* Uniblox - importer for Roblox binary place/model files (.rbxl / .rbxm).
 *
 * Reads the documented binary format (see rbx-dom's binary.md): chunks
 * compressed with LZ4 or Zstandard, INST/PROP/PRNT/SSTR/META/END. Every
 * instance is kept (class name and Name), and these become real Uniblox
 * objects:
 *   Workspace, Lighting, Folder, Model
 *   Part (block / ball / cylinder / wedge / corner wedge), WedgePart,
 *   CornerWedgePart, TrussPart, SpawnLocation, Seat, VehicleSeat
 *   MeshPart / UnionOperation: a box of the right size and colour, since
 *     their geometry lives on Roblox's servers or in CSG data we do not
 *     read
 * Read part properties: CFrame, Size, Color (Color3uint8 or BrickColor),
 * Material, Shape, Transparency, Anchored, CanCollide. Lighting: ClockTime
 * or TimeOfDay, GeographicLatitude, Brightness, Ambient, OutdoorAmbient,
 * FogEnd, FogColor. Workspace: Gravity, FallenPartsDestroyHeight. Joints
 * (Weld, ManualWeld, Snap, Glue, Motor, Motor6D, WeldConstraint) become
 * rigid welds; motors do not turn yet. Scripts keep their code (Source),
 * Disabled and RunContext. Terrain voxels, decals and textures are not
 * imported.
 *
 * Positions and rotations are converted from Roblox's right-handed axes
 * (+Z towards the viewer) to Uniblox's left-handed ones (+Z forward).
 * The input is untrusted. Nothing is ever downloaded. */
#ifndef UB_RBXL_H
#define UB_RBXL_H

#include "world/ub_world.h"

typedef struct {
    u32 instances;        /* in the file */
    u32 parts;            /* imported as visible parts */
    u32 placeholders;     /* meshes/unions shown as boxes */
    u32 meshes;           /* meshes/unions shown with the geometry in the file (8C) */
    u32 invisible;        /* parts with Transparency ~1, kept but not drawn */
    u32 terrain;          /* Terrain instances (voxels not imported) */
    u32 classes;          /* distinct classes */
    u32 welds;            /* joints imported as welds */
    u32 joints_skipped;   /* disabled, or naming something that is not a part */
    u32 scripts;          /* Script, LocalScript and ModuleScript, with their code */
    u32 gui;              /* GUI objects (ScreenGui, Frame, TextLabel...) with their properties */
    char compression[32]; /* e.g. "zstd+lz4" */
} ub_import_stats;

/* True if the data starts with the Roblox binary signature. */
bool ub_rbxl_detect(const u8 *data, u32 size);

/* Import into `w` (freshly initialised). */
ub_load_result ub_rbxl_load(ub_world *w, const u8 *data, u32 size, ub_import_stats *stats);

/* The rotation matrix (row-major R00..R22) for a CFrame special-case ID
 * (0x02..0x23), as stored in Roblox files. False for invalid IDs. */
bool ub_rbxl_special_rotation(u32 id, f32 out[9]);

#endif
