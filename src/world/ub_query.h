/* Uniblox - spatial queries on the world.
 *
 * Rays are tested against each part's real shape (box, wedge, corner
 * wedge, and the low-detail cylinder and ball), not just its bounding box.
 * Brute force over all parts: fine for tools and tests. The game itself
 * uses ub_phys_raycast (physics/ub_physics.h), which has a spatial index. */
#ifndef UB_QUERY_H
#define UB_QUERY_H

#include "world/ub_world.h"

typedef struct {
    f32 t;            /* distance along the ray (ray direction is normalised) */
    vec3 point;
    i32 instance;     /* index into w->inst */
} ub_ray_hit;

/* Nearest collidable part hit by the ray within max_dist. Only parts under
 * `root` (-1 = anywhere) with UB_PART_CAN_COLLIDE count. */
bool ub_world_raycast(const ub_world *w, i32 root, vec3 origin, vec3 dir, f32 max_dist, ub_ray_hit *hit);

/* Ray against one part: entry distance, or false if missed. A ray starting
 * inside the part hits at t = 0. */
bool ub_part_raycast(const ub_part *p, vec3 origin, vec3 dir, f32 max_dist, f32 *t);

#endif
