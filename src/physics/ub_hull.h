/* Uniblox - convex hulls of the part shapes, for collision.
 *
 * Each hull lives in the unit cube (-0.5..0.5) like the render meshes and
 * is scaled by the part's size. Faces are wound counter-clockwise seen
 * from outside; every edge knows the two faces it joins (for the
 * separating-axis test). Balls are not hulls: physics treats them as true
 * spheres. Cylinders have 12 sides (within 3.4% of round). */
#ifndef UB_HULL_H
#define UB_HULL_H

#include "core/ub_vec.h"
#include "render/ub_mesh.h"

#define UB_HULL_MAX_VERTS 32
#define UB_HULL_MAX_FACES 18
#define UB_HULL_MAX_EDGES 48
#define UB_HULL_MAX_FACE_VERTS 96 /* all faces' vertex lists together */

typedef struct {
    u8 a, b;           /* vertex indices */
    u8 f0, f1;         /* the two faces that share this edge */
} ub_hull_edge;

typedef struct {
    u8 first, count;   /* range in fv[] */
} ub_hull_face;

typedef struct {
    int vert_count, face_count, edge_count;
    vec3 v[UB_HULL_MAX_VERTS];
    vec3 n[UB_HULL_MAX_FACES];          /* outward unit normals (unit cube) */
    ub_hull_face f[UB_HULL_MAX_FACES];
    u8 fv[UB_HULL_MAX_FACE_VERTS];
    ub_hull_edge e[UB_HULL_MAX_EDGES];
    /* mass properties of the unit-cube solid, density 1 */
    f32 volume;
    vec3 centroid;
    mat3 second_moment;                 /* integral of r rᵀ dV about the origin */
} ub_hull;

/* The hull for a shape; balls (and unknown shapes) get the box. */
const ub_hull *ub_hull_get(ub_shape s);

/* Mass properties of a part shape of this size, density 1: volume, centre
 * of mass and inertia tensor about the centre of mass, in part space. */
void ub_shape_mass(ub_shape s, vec3 size, f32 *volume, vec3 *centroid, mat3 *inertia);

#endif
