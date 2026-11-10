/* Uniblox - meshes for the basic part shapes.
 *
 * Each shape is a convex polyhedron in a unit cube (-0.5..+0.5 on every
 * axis), made of flat convex faces. Parts scale them by their size.
 * Two detail levels: LOW (12-sided cylinders, 6x10 balls) for the software
 * renderer, HIGH (32 sides, 16x32) for OpenGL. Curved faces are marked
 * smooth and use per-vertex normals so they shade as one round surface. */
#ifndef UB_MESH_H
#define UB_MESH_H

#include "core/ub_vec.h"

typedef enum {
    UB_SHAPE_BOX,
    UB_SHAPE_WEDGE,     /* ramp rising towards +Z */
    UB_SHAPE_CYLINDER,  /* axis along Y */
    UB_SHAPE_BALL,
    UB_SHAPE_CORNER_WEDGE, /* pyramid with its apex above the (-X, +Z) corner */
    UB_SHAPE_COUNT
} ub_shape;

typedef enum { UB_DETAIL_LOW, UB_DETAIL_HIGH, UB_DETAIL_COUNT } ub_detail;

#define UB_MESH_MAX_VERTS 512
#define UB_MESH_MAX_FACES 512
#define UB_FACE_MAX_VERTS 32

typedef struct {
    u8 count;
    bool smooth;              /* use vnormals[] instead of normal */
    u16 v[UB_FACE_MAX_VERTS]; /* vertex indices; the first edge sets the
                                 texture's u direction */
    vec3 normal;              /* outward, unit length, in the unit cube */
} ub_face;

/* The shapes have at most UB_MESH_MAX_VERTS / UB_MESH_MAX_FACES; meshes
 * made from parts' own geometry (ub_mesh_from_triangles) can have more. */
typedef struct ub_mesh_s {
    int vert_count, face_count;
    vec3 *verts;
    vec3 *vnormals;           /* smooth-shading normals (unit cube) */
    ub_face *faces;
} ub_mesh;

void ub_meshes_init(void);
const ub_mesh *ub_mesh_get(ub_shape s, ub_detail d);

/* A mesh of flat triangles in the unit cube, made of convex pieces
 * (piece_v / piece_t: each piece's end vertex and end triangle), with each
 * face turned to face out of its piece. One allocation: plat_free it.
 * NULL if out of memory. */
ub_mesh *ub_mesh_from_triangles(const f32 *v, u32 nv, const u32 *tri, u32 nt, const u32 *piece_v,
                                const u32 *piece_t, u32 npieces);

#endif
