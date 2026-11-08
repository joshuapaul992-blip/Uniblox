/* Uniblox - meshes for the basic part shapes.
 *
 * Each shape is a convex polyhedron in a unit cube (-0.5..+0.5 on every
 * axis), made of flat convex faces. Parts scale them by their size. */
#ifndef UB_MESH_H
#define UB_MESH_H

#include "core/ub_vec.h"

typedef enum {
    UB_SHAPE_BOX,
    UB_SHAPE_WEDGE,     /* ramp rising towards +Z */
    UB_SHAPE_CYLINDER,  /* axis along Y */
    UB_SHAPE_BALL,
    UB_SHAPE_COUNT
} ub_shape;

#define UB_MESH_MAX_VERTS 64
#define UB_MESH_MAX_FACES 64
#define UB_FACE_MAX_VERTS 12

typedef struct {
    u8 count;
    u8 v[UB_FACE_MAX_VERTS];  /* vertex indices; the first edge sets the
                                 texture's u direction */
    vec3 normal;              /* outward, unit length, in the unit cube */
} ub_face;

typedef struct {
    int vert_count, face_count;
    vec3 verts[UB_MESH_MAX_VERTS];
    ub_face faces[UB_MESH_MAX_FACES];
} ub_mesh;

void ub_meshes_init(void);
const ub_mesh *ub_mesh_get(ub_shape s);

#endif
