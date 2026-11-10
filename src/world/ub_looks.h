/* Uniblox - looks of MeshParts and parts with SpecialMeshes (see ub_looks.c). */
#ifndef UB_LOOKS_H
#define UB_LOOKS_H

#include "world/ub_world.h"

/* Gives parts their looks from mesh files in content/ and from the
 * SpecialMesh, BlockMesh and CylinderMesh inside them. Returns how many
 * parts got one. */
int ub_world_resolve_looks(ub_world *w);

#endif
