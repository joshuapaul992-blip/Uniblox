/* Uniblox - Roblox .mesh files (see ub_rbxmesh.c). */
#ifndef UB_RBXMESH_H
#define UB_RBXMESH_H

#include "core/ub_types.h"

/* Positions (3 floats each, the file's own units and axes) and triangles
 * (3 indices each) of a .mesh file's most detailed level. One allocation:
 * plat_free(*v) frees both. False if it is not one or is damaged. */
bool ub_rbxmesh_parse(const u8 *data, u32 len, f32 **v, u32 *nv, u32 **tri, u32 *nt);

#endif
