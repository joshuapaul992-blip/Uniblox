/* Uniblox - contact generation between two collision shapes.
 *
 * Hull vs hull uses the separating-axis test over face normals and edge
 * pairs (only edge pairs that form a face of the Minkowski difference),
 * then clips the most anti-parallel face against the reference face to
 * get up to four contact points. Spheres use closest points. Contacts
 * closer than `margin` are reported too (with positive separation), so the
 * solver can stop things before they overlap. */
#ifndef UB_COLLIDE_H
#define UB_COLLIDE_H

#include "core/ub_vec.h"
#include "physics/ub_hull.h"

typedef struct {
    bool sphere;          /* true: a sphere of `radius`; false: a hull */
    const ub_hull *hull;
    vec3 pos;             /* world centre */
    mat3 rot;
    vec3 size;
    f32 radius;
} ub_cshape;

typedef struct {
    vec3 point;           /* world, halfway between the surfaces */
    f32 sep;              /* separation along the normal; < 0 = overlapping */
} ub_cpoint;

#define UB_MAX_CONTACTS 4

/* Contacts between a and b; `normal` points from a towards b. Returns the
 * number of points (0 = apart by more than margin). */
int ub_collide(const ub_cshape *a, const ub_cshape *b, f32 margin, vec3 *normal, ub_cpoint out[UB_MAX_CONTACTS]);

/* Ray against one shape: entry distance and surface normal. A ray that
 * starts inside hits at t = 0 with the normal of the nearest face. */
bool ub_shape_raycast(const ub_cshape *s, vec3 origin, vec3 dir, f32 max_dist, f32 *t, vec3 *normal);

#endif
