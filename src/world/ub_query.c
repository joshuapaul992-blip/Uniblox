#include "world/ub_query.h"
#include "render/ub_mesh.h"

/* World -> the part's unit-cube space: undo position, rotation, then size
 * (an affine map, so distances along the ray keep the same parameter). */
static vec3 to_unit(const ub_part *p, vec3 v, bool is_dir)
{
    vec3 d = is_dir ? v : v3_sub(v, p->pos);
    vec3 l = v3(v3_dot(d, p->rot.x), v3_dot(d, p->rot.y), v3_dot(d, p->rot.z)); /* rot is orthonormal */
    vec3 s = v3(p->size.x > 1e-4f ? p->size.x : 1e-4f, p->size.y > 1e-4f ? p->size.y : 1e-4f,
                p->size.z > 1e-4f ? p->size.z : 1e-4f);
    return v3(l.x / s.x, l.y / s.y, l.z / s.z);
}

bool ub_part_raycast(const ub_part *p, vec3 origin, vec3 dir, f32 max_dist, f32 *t)
{
    /* quick reject with the bounding sphere */
    f32 r = 0.5f * v3_length(p->size);
    vec3 oc = v3_sub(p->pos, origin);
    f32 along = v3_dot(oc, dir);
    f32 dist2 = v3_dot(oc, oc) - along * along;
    if (dist2 > r * r || along < -r || along - r > max_dist) return false;

    /* Cyrus-Beck: clip the ray against every face plane of the convex mesh */
    const ub_mesh *m = ub_mesh_get((ub_shape)p->shape, UB_DETAIL_LOW);
    vec3 o = to_unit(p, origin, false), d = to_unit(p, dir, true);
    f32 t_in = 0.0f, t_out = max_dist;
    for (int f = 0; f < m->face_count; ++f) {
        vec3 n = m->faces[f].normal;
        f32 plane = v3_dot(n, m->verts[m->faces[f].v[0]]);
        f32 num = plane - v3_dot(n, o), den = v3_dot(n, d);
        if (den > -1e-9f && den < 1e-9f) {
            if (num < 0) return false; /* parallel and outside */
            continue;
        }
        f32 tt = num / den;
        if (den < 0) { if (tt > t_in) t_in = tt; }
        else if (tt < t_out) t_out = tt;
        if (t_in > t_out) return false;
    }
    *t = t_in;
    return true;
}

bool ub_world_raycast(const ub_world *w, i32 root, vec3 origin, vec3 dir, f32 max_dist, ub_ray_hit *hit)
{
    bool found = false;
    f32 best = max_dist;
    for (i32 i = 0; i < w->count; ++i) {
        const ub_instance *in = &w->inst[i];
        if (in->part < 0) continue;
        const ub_part *p = &w->parts[in->part];
        if (!(p->flags & UB_PART_CAN_COLLIDE)) continue;
        if (root >= 0 && !ub_world_is_descendant(w, i, root)) continue;
        f32 t;
        if (ub_part_raycast(p, origin, dir, best, &t) && t <= best) {
            best = t;
            hit->t = t;
            hit->instance = i;
            found = true;
        }
    }
    if (found) hit->point = v3_add(origin, v3_scale(dir, hit->t));
    return found;
}
