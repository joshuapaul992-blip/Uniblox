/* Uniblox - how parts with a mesh look (milestone 8C).
 *
 * After a place is imported: a MeshPart whose mesh file the player has put
 * in the content folder (content/<asset id>.mesh) is drawn with it, fitted
 * to the part's size as Roblox does. A part with a SpecialMesh,
 * BlockMesh or CylinderMesh inside is drawn as that mesh, with its Scale
 * and Offset; a SpecialMesh's FileMesh is used at its own size times
 * Scale. Collision stays the part's own, as in Roblox. Nothing is
 * downloaded: a mesh that is not there leaves the part as it was. */
#include "world/ub_looks.h"
#include "core/ub_path.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "render/ub_mesh.h"
#include "world/ub_rbxmesh.h"
#include "crt/ub_crt.h"

static bool streq(const char *a, const char *b) { return ub_strcmp(a, b) == 0; }

static vec3 v3_prop(const ub_world *w, i32 i, const char *k, vec3 dflt)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_VECTOR3 ? v3(v->v.v3.x, v->v.v3.y, v->v.v3.z) : dflt;
}

static const char *str_prop(const ub_world *w, i32 i, const char *k)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_STRING && v->v.str.len ? v->v.str.s : NULL;
}

/* A mesh file from the content folder: positions and triangles (one
 * allocation, plat_free(*v)). Remembers files that are missing. */
typedef struct {
    char path[UB_PATH_MAX + 16];
    bool missing;
} mcache;

static bool load_mesh(const char *content, f32 **v, u32 *nv, u32 **tri, u32 *nt, mcache *miss, int *nmiss)
{
    char path[UB_PATH_MAX + 16];
    if (!ub_content_path(content, path, (int)sizeof path, "mesh") || !ub_path_is_safe(path)) return false;
    for (int i = 0; i < *nmiss; ++i)
        if (streq(miss[i].path, path)) return false;
    void *data = NULL;
    u32 size = 0;
    bool ok = plat_file_exists(path) && plat_file_read_all(path, 64u << 20, &data, &size) &&
              ub_rbxmesh_parse((const u8 *)data, size, v, nv, tri, nt);
    plat_free(data);
    if (!ok && *nmiss < 64) {
        ub_fmt(miss[*nmiss].path, (int)sizeof miss[*nmiss].path, "%s", path);
        (*nmiss)++;
    }
    return ok;
}

/* Into the part's unit cube: v' = (v * scale + offset) / div - shift, z
 * flipped (Roblox's axes to Uniblox's); triangles turned to match. */
static u16 add_look(ub_world *w, f32 *v, u32 nv, u32 *tri, u32 nt, vec3 scale, vec3 offset, vec3 div, bool fit)
{
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (u32 i = 0; i < nv; ++i) {
        f32 *p = v + i * 3;
        lo = v3(UB_MIN(lo.x, p[0]), UB_MIN(lo.y, p[1]), UB_MIN(lo.z, p[2]));
        hi = v3(UB_MAX(hi.x, p[0]), UB_MAX(hi.y, p[1]), UB_MAX(hi.z, p[2]));
    }
    vec3 mid = v3_scale(v3_add(lo, hi), 0.5f), ext = v3_sub(hi, lo);
    if (fit) { /* MeshParts: the mesh's box becomes the part's */
        div = v3(UB_MAX(ext.x, 1e-4f), UB_MAX(ext.y, 1e-4f), UB_MAX(ext.z, 1e-4f));
        offset = v3_scale(mid, -1.0f);
        scale = v3(1, 1, 1);
    }
    for (u32 i = 0; i < nv; ++i) {
        f32 *p = v + i * 3;
        p[0] = (p[0] * scale.x + offset.x) / div.x;
        p[1] = (p[1] * scale.y + offset.y) / div.y;
        p[2] = -(p[2] * scale.z + offset.z) / div.z;
    }
    for (u32 t = 0; t < nt; ++t) { /* the mirror turns the winding round */
        u32 b = tri[t * 3 + 1];
        tri[t * 3 + 1] = tri[t * 3 + 2];
        tri[t * 3 + 2] = b;
    }
    u32 pv = nv, pt = nt;
    return ub_world_add_mesh(w, v, nv, tri, nt, &pv, &pt, 1, UB_WMESH_WOUND);
}

/* A basic shape (render/ub_mesh.h) scaled and moved within the part. */
static u16 shape_look(ub_world *w, ub_shape shape, vec3 scale, vec3 offset_unit, bool turn)
{
    const ub_mesh *m = ub_mesh_get(shape, UB_DETAIL_HIGH);
    u32 nt = 0;
    for (int f = 0; f < m->face_count; ++f) nt += (u32)(m->faces[f].count - 2);
    f32 *v = (f32 *)plat_alloc(sizeof(f32) * (size_t)m->vert_count * 3 + sizeof(u32) * nt * 3);
    if (!v) return 0;
    u32 *tri = (u32 *)(v + m->vert_count * 3);
    for (int i = 0; i < m->vert_count; ++i) {
        vec3 p = m->verts[i];
        if (turn) p = v3(-p.x, p.y, -p.z); /* Roblox's wedge rises the other way */
        v[i * 3] = p.x * scale.x + offset_unit.x;
        v[i * 3 + 1] = p.y * scale.y + offset_unit.y;
        v[i * 3 + 2] = p.z * scale.z + offset_unit.z;
    }
    u32 k = 0;
    for (int f = 0; f < m->face_count; ++f)
        for (int j = 1; j + 1 < m->faces[f].count; ++j, ++k) {
            tri[k * 3] = m->faces[f].v[0];
            tri[k * 3 + 1] = m->faces[f].v[j];
            tri[k * 3 + 2] = m->faces[f].v[j + 1];
        }
    u32 pv = (u32)m->vert_count, pt = nt;
    u16 id = ub_world_add_mesh(w, v, (u32)m->vert_count, tri, nt, &pv, &pt, 1, 0); /* one convex piece */
    plat_free(v);
    return id;
}

int ub_world_resolve_looks(ub_world *w)
{
    mcache *miss = (mcache *)plat_alloc(sizeof(mcache) * 64);
    int nmiss = 0, done = 0;
    if (!miss) return 0;
    for (i32 i = 0; i < w->count; ++i) {
        if (!ub_world_alive(w, i)) continue;
        const char *cls = ub_world_str(w, w->inst[i].class_name);
        i32 pi = w->inst[i].part;
        if (pi >= 0 && streq(cls, "MeshPart")) {
            const char *id = str_prop(w, i, "MeshId");
            if (!id) id = str_prop(w, i, "Mesh");
            f32 *v;
            u32 nv, nt, *tri;
            if (id && load_mesh(id, &v, &nv, &tri, &nt, miss, &nmiss)) {
                u16 look = add_look(w, v, nv, tri, nt, v3(1, 1, 1), v3(0, 0, 0), v3(1, 1, 1), true);
                plat_free(v);
                if (look) {
                    w->parts[pi].look = look;
                    w->parts[pi].flags &= (u8)~UB_PART_PLACEHOLDER;
                    done++;
                }
            }
            continue;
        }
        bool special = streq(cls, "SpecialMesh"), block = streq(cls, "BlockMesh"), cyl = streq(cls, "CylinderMesh");
        if (!special && !block && !cyl) continue;
        i32 parent = w->inst[i].parent;
        if (parent < 0 || w->inst[parent].part < 0) continue;
        ub_part *p = &w->parts[w->inst[parent].part];
        if (p->look) continue; /* the first mesh inside counts */
        vec3 scale = v3_prop(w, i, "Scale", v3(1, 1, 1)), offset = v3_prop(w, i, "Offset", v3(0, 0, 0));
        vec3 size = v3(UB_MAX(p->size.x, 1e-3f), UB_MAX(p->size.y, 1e-3f), UB_MAX(p->size.z, 1e-3f));
        /* MeshType: Head 0, Torso 1, Wedge 2, Sphere 3, Cylinder 4, FileMesh 5, Brick 6, the rest boxes */
        int type = block ? 6 : cyl ? 4 : 6;
        if (special) {
            const ub_value *mt = ub_world_prop_named(w, i, "MeshType");
            type = mt && mt->type == UB_PV_ENUM ? (int)mt->v.e : 6;
        }
        u16 look = 0;
        if (type == 5) {
            const char *id = str_prop(w, i, "MeshId");
            f32 *v;
            u32 nv, nt, *tri;
            if (id && load_mesh(id, &v, &nv, &tri, &nt, miss, &nmiss)) {
                look = add_look(w, v, nv, tri, nt, scale, offset, size, false);
                plat_free(v);
            }
        } else {
            vec3 off = v3(offset.x / size.x, offset.y / size.y, -offset.z / size.z);
            bool plain = scale.x == 1 && scale.y == 1 && scale.z == 1 && offset.x == 0 && offset.y == 0 && offset.z == 0;
            ub_shape shape = type == 2 ? UB_SHAPE_WEDGE : type == 3 ? UB_SHAPE_BALL : (type == 4 || type == 0) ? UB_SHAPE_CYLINDER
                           : type == 11 ? UB_SHAPE_CORNER_WEDGE : UB_SHAPE_BOX;
            if (shape == UB_SHAPE_BALL && p->shape == UB_SHAPE_BALL && plain) continue;
            if (shape == UB_SHAPE_BOX && p->shape == UB_SHAPE_BOX && plain) continue; /* looks the same */
            look = shape_look(w, shape, scale, off, shape == UB_SHAPE_WEDGE || shape == UB_SHAPE_CORNER_WEDGE);
        }
        if (look) {
            p->look = look;
            done++;
        }
    }
    plat_free(miss);
    return done;
}
