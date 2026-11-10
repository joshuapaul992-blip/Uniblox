#include "world/ub_rbxl.h"
#include "core/ub_compress.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "crt/ub_crt.h"

#define MAX_INSTANCES (4u * 1024u * 1024u)
#define MAX_CHUNK (256u * 1024u * 1024u)

/* ------------------------------------------------------------- tables */

bool ub_rbxl_special_rotation(u32 id, f32 out[9])
{
    /* id - 1 = 6a + b: column 0 is axis a, column 1 is axis b, column 2 is
     * their cross product; axes 0..5 = +X +Y +Z -X -Y -Z. Checked against
     * all 24 cases in rbx-test-files' cframe-special-cases. */
    static const f32 axis[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } };
    if (id < 2 || id > 0x23) return false;
    u32 a = (id - 1) / 6, b = (id - 1) % 6;
    if (a % 3 == b % 3) return false; /* parallel axes: not a rotation */
    const f32 *x = axis[a], *y = axis[b];
    f32 z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    for (int r = 0; r < 3; ++r) {
        out[r * 3 + 0] = x[r];
        out[r * 3 + 1] = y[r];
        out[r * 3 + 2] = z[r];
    }
    return true;
}

typedef struct { u16 number; u8 r, g, b; } brick_color;

/* The common BrickColor numbers (modern files store Color3uint8 instead). */
static const brick_color k_brick[] = {
    { 1, 242, 243, 243 }, { 2, 161, 165, 162 }, { 5, 215, 197, 154 }, { 18, 204, 142, 105 },
    { 21, 196, 40, 28 }, { 23, 13, 105, 172 }, { 24, 245, 205, 48 }, { 26, 27, 42, 53 },
    { 28, 40, 127, 71 }, { 29, 161, 196, 140 }, { 37, 75, 151, 75 }, { 38, 160, 95, 53 },
    { 45, 180, 210, 228 }, { 101, 218, 134, 122 }, { 102, 110, 153, 202 }, { 104, 107, 50, 124 },
    { 105, 226, 155, 64 }, { 106, 218, 133, 65 }, { 107, 0, 143, 156 }, { 119, 164, 189, 71 },
    { 125, 234, 184, 146 }, { 135, 116, 134, 157 }, { 141, 39, 70, 45 }, { 151, 120, 144, 130 },
    { 153, 149, 121, 119 }, { 192, 105, 64, 40 }, { 194, 163, 162, 165 }, { 199, 99, 95, 98 },
    { 208, 229, 228, 223 }, { 217, 124, 92, 70 }, { 226, 253, 234, 141 }, { 1001, 248, 248, 248 },
    { 1002, 205, 205, 205 }, { 1003, 17, 17, 17 }, { 1004, 255, 0, 0 }, { 1005, 255, 176, 0 },
    { 1006, 180, 128, 255 }, { 1007, 163, 75, 75 }, { 1008, 193, 190, 66 }, { 1009, 255, 255, 0 },
    { 1010, 0, 0, 255 }, { 1011, 0, 32, 96 }, { 1012, 33, 84, 185 }, { 1013, 4, 175, 236 },
    { 1014, 170, 85, 0 }, { 1015, 170, 0, 170 }, { 1016, 255, 102, 204 }, { 1017, 255, 175, 0 },
    { 1018, 18, 238, 212 }, { 1019, 0, 255, 255 }, { 1020, 0, 255, 0 }, { 1021, 58, 125, 21 },
    { 1022, 127, 142, 100 }, { 1023, 140, 91, 159 }, { 1024, 175, 221, 255 }, { 1025, 255, 201, 201 },
    { 1026, 177, 167, 255 }, { 1027, 159, 243, 233 }, { 1028, 204, 255, 204 }, { 1029, 255, 255, 204 },
    { 1030, 255, 204, 153 }, { 1031, 98, 37, 209 }, { 1032, 255, 0, 191 },
};

static u32 brick_to_rgb(u32 n)
{
    for (int i = 0; i < UB_ARRAY_COUNT(k_brick); ++i)
        if (k_brick[i].number == n)
            return ((u32)k_brick[i].r << 16) | ((u32)k_brick[i].g << 8) | k_brick[i].b;
    return 0xA3A2A5; /* Medium stone grey, Roblox's default */
}

/* Roblox Enum.Material values -> Uniblox materials. */
static u8 map_material(u32 m, bool *glow)
{
    *glow = m == 288; /* Neon */
    if (m == 512 || m == 528) return UB_MAT_WOOD;               /* Wood, WoodPlanks */
    if (m == 848) return UB_MAT_BRICK;                          /* Brick */
    if (m >= 784 && m <= 940) return UB_MAT_STONE;              /* Marble..Sandstone */
    if (m >= 1040 && m <= 1100) return UB_MAT_METAL;            /* CorrodedMetal..Metal */
    if (m == 1280 || m == 1284 || m == 1344 || m == 1360) return UB_MAT_GRASS; /* Grass, LeafyGrass, Mud, Ground */
    if (m == 1296 || m == 1328 || m == 1376 || m == 1392) return UB_MAT_STONE; /* Sand, Snow, Asphalt, Salt */
    return UB_MAT_PLASTIC;                                      /* Plastic, SmoothPlastic, Glass, ... */
}

/* ------------------------------------------------------------- reading */

typedef struct {
    const u8 *p;
    u32 len, pos;
    bool bad;
} rd;

static bool need(rd *r, u32 n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static u32 le32(const u8 *q) { return q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24); }

static u32 rd_u32(rd *r)
{
    if (!need(r, 4)) return 0;
    u32 v = le32(r->p + r->pos);
    r->pos += 4;
    return v;
}

static u8 rd_u8(rd *r)
{
    if (!need(r, 1)) return 0;
    return r->p[r->pos++];
}

/* String: u32 length + bytes. Returns a pointer into the buffer. */
static const u8 *rd_str(rd *r, u32 *n)
{
    *n = rd_u32(r);
    if (!need(r, *n)) return NULL;
    const u8 *s = r->p + r->pos;
    r->pos += *n;
    return s;
}

/* Interleaved array of `count` big-endian u32s starting at the cursor. */
static bool rd_interleaved32(rd *r, u32 count, u32 *out)
{
    if (count > (r->len - r->pos) / 4 || !need(r, count * 4)) {
        r->bad = true;
        return false;
    }
    const u8 *q = r->p + r->pos;
    for (u32 i = 0; i < count; ++i)
        out[i] = ((u32)q[i] << 24) | ((u32)q[count + i] << 16) | ((u32)q[2 * count + i] << 8) | q[3 * count + i];
    r->pos += count * 4;
    return true;
}

/* Three separately interleaved arrays of n values (X..., Y..., Z...), as
 * Vector3, Color3 and CFrame positions are stored. */
static bool rd_interleaved32x3(rd *r, u32 n, u32 *out)
{
    return rd_interleaved32(r, n, out) && rd_interleaved32(r, n, out + n) && rd_interleaved32(r, n, out + 2 * n);
}

static i32 untransform(u32 v) { return (i32)((v >> 1) ^ (0u - (v & 1))); }

static f32 roblox_float(u32 v)
{
    u32 ieee = (v >> 1) | (v << 31); /* the sign bit is stored last */
    f32 f;
    memcpy(&f, &ieee, 4);
    return f;
}

/* -------------------------------------------------------------- import */

typedef enum {
    K_OTHER, K_WORKSPACE, K_LIGHTING, K_FOLDER, K_MODEL,
    K_PART, K_WEDGE, K_CORNER, K_TRUSS, K_SPAWN, K_SEAT, K_MESH, K_NEGATE, K_TERRAIN,
    K_SCRIPT, K_LOCALSCRIPT, K_MODULESCRIPT,
    K_JOINT,             /* Weld, Snap, Motor6D...: Part0 + Part1 */
    K_WELD_CONSTRAINT,   /* WeldConstraint: Part0Internal + Part1Internal */
} kind;

typedef struct {
    u32 id;
    char name[48];
    kind k;
    u32 count;
    i32 first_rec;       /* records of this class are contiguous */
} cls;

typedef struct {
    i32 ref;
    i32 cls;
    u32 name;            /* offset into the temp name pool, 0xFFFFFFFF = none */
    i32 parent;          /* record index, -1 = root */
    i32 first_child, last_child, next_sibling;
    f32 rot[9];
    vec3 pos, size;
    u32 color;
    bool has_color3;
    u32 brick;
    u32 material;
    u32 shape;           /* Roblox PartType: 0 ball, 1 block, 2 cylinder, 3 wedge, 4 corner wedge */
    f32 transparency;
    u8 anchored, can_collide;
    bool parented;
    i32 part0, part1;    /* joints: referents of the joined parts, -1 = none */
    u8 joint_on;         /* joints: Enabled */
    u32 source;          /* scripts: offset of their code in the name pool, 0xFFFFFFFF = none */
    u8 disabled;         /* scripts */
    u8 run_context;      /* scripts: 0 legacy, 1 server, 2 client */
} rec;

typedef struct {
    cls *classes;
    u32 class_count, class_cap;
    rec *recs;
    u32 rec_count, rec_cap;   /* rec_cap = the header's instance count */
    i32 *map_keys, *map_vals;   /* referent -> record index (open addressing) */
    u32 map_cap;
    char *names;
    u32 names_len, names_cap;
    ub_lighting light;
    f32 gravity, fallen_height;
    bool used_zstd, used_lz4;
} importer;

static void map_put(importer *im, i32 ref, i32 idx)
{
    u32 h = ((u32)ref * 2654435761u) & (im->map_cap - 1);
    while (im->map_keys[h] != -1 && im->map_keys[h] != ref) h = (h + 1) & (im->map_cap - 1);
    im->map_keys[h] = ref;
    im->map_vals[h] = idx;
}

static i32 map_get(const importer *im, i32 ref)
{
    if (ref < 0 || !im->map_cap) return -1;
    u32 h = ((u32)ref * 2654435761u) & (im->map_cap - 1);
    for (u32 probes = 0; probes < im->map_cap; ++probes) {
        if (im->map_keys[h] == -1) return -1;
        if (im->map_keys[h] == ref) return im->map_vals[h];
        h = (h + 1) & (im->map_cap - 1);
    }
    return -1;
}

static u32 add_text(importer *im, const u8 *s, u32 n, bool keep_nul);

static u32 add_name(importer *im, const u8 *s, u32 n)
{
    if (n > 200) n = 200;
    return add_text(im, s, n, false);
}

/* Appends text to the name pool (NULs become '?' unless kept). */
static u32 add_text(importer *im, const u8 *s, u32 n, bool keep_nul)
{
    if (im->names_len + n + 1 > im->names_cap) {
        u32 nc = im->names_cap ? im->names_cap * 2 : 65536;
        while (nc < im->names_len + n + 1) nc *= 2;
        char *np = (char *)plat_alloc(nc);
        if (!np) return 0xFFFFFFFFu;
        if (im->names) {
            memcpy(np, im->names, im->names_len);
            plat_free(im->names);
        }
        im->names = np;
        im->names_cap = nc;
    }
    u32 off = im->names_len;
    for (u32 i = 0; i < n; ++i) im->names[off + i] = s[i] || keep_nul ? (char)s[i] : '?';
    im->names[off + n] = 0;
    im->names_len += n + 1;
    return off;
}

static kind classify(const char *n)
{
    static const struct { const char *name; kind k; } table[] = {
        { "Workspace", K_WORKSPACE }, { "Lighting", K_LIGHTING }, { "Folder", K_FOLDER },
        { "Model", K_MODEL }, { "Part", K_PART }, { "WedgePart", K_WEDGE },
        { "CornerWedgePart", K_CORNER }, { "TrussPart", K_TRUSS }, { "SpawnLocation", K_SPAWN },
        { "Seat", K_SEAT }, { "VehicleSeat", K_SEAT }, { "SkateboardPlatform", K_SEAT },
        { "MeshPart", K_MESH }, { "UnionOperation", K_MESH }, { "IntersectOperation", K_MESH },
        { "PartOperation", K_MESH }, { "NegateOperation", K_NEGATE }, { "Terrain", K_TERRAIN },
        /* rigid joints; motors and Motor6D are held still for now */
        { "Weld", K_JOINT }, { "ManualWeld", K_JOINT }, { "Snap", K_JOINT }, { "Glue", K_JOINT },
        { "ManualGlue", K_JOINT }, { "Motor", K_JOINT }, { "Motor6D", K_JOINT }, { "VelocityMotor", K_JOINT },
        { "WeldConstraint", K_WELD_CONSTRAINT },
        { "Script", K_SCRIPT }, { "LocalScript", K_LOCALSCRIPT }, { "ModuleScript", K_MODULESCRIPT },
    };
    for (int i = 0; i < UB_ARRAY_COUNT(table); ++i) {
        const char *a = n, *b = table[i].name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) return table[i].k;
    }
    return K_OTHER;
}

static bool is_part_kind(kind k)
{
    return k == K_PART || k == K_WEDGE || k == K_CORNER || k == K_TRUSS || k == K_SPAWN || k == K_SEAT ||
           k == K_MESH || k == K_NEGATE;
}

static cls *find_class(importer *im, u32 id)
{
    for (u32 i = 0; i < im->class_count; ++i)
        if (im->classes[i].id == id) return &im->classes[i];
    return NULL;
}

static bool name_is(const u8 *s, u32 n, const char *lit)
{
    u32 i = 0;
    for (; i < n && lit[i]; ++i)
        if ((char)s[i] != lit[i]) return false;
    return i == n && !lit[i];
}

static ub_load_result chunk_inst(importer *im, rd *r)
{
    u32 id = rd_u32(r), nl;
    const u8 *name = rd_str(r, &nl);
    rd_u8(r); /* object format: service or not */
    u32 count = rd_u32(r);
    if (r->bad || !name || count > im->rec_cap - im->rec_count) return UB_LOAD_CORRUPT;
    if (find_class(im, id)) return UB_LOAD_CORRUPT;
    if (im->class_count == im->class_cap) {
        u32 nc = im->class_cap ? im->class_cap * 2 : 64;
        cls *np = (cls *)plat_alloc(sizeof(cls) * nc);
        if (!np) return UB_LOAD_NO_MEMORY;
        if (im->classes) {
            memcpy(np, im->classes, sizeof(cls) * im->class_count);
            plat_free(im->classes);
        }
        im->classes = np;
        im->class_cap = nc;
    }
    cls *c = &im->classes[im->class_count++];
    memset(c, 0, sizeof *c);
    c->id = id;
    u32 cn = nl < sizeof c->name - 1 ? nl : (u32)sizeof c->name - 1;
    for (u32 i = 0; i < cn; ++i) c->name[i] = (char)name[i];
    c->k = classify(c->name);
    c->count = count;

    /* records are allocated up front for the header's instance count */
    u32 *refs = (u32 *)plat_alloc(sizeof(u32) * (count ? count : 1));
    if (!refs) return UB_LOAD_NO_MEMORY;
    if (!rd_interleaved32(r, count, refs)) {
        plat_free(refs);
        return UB_LOAD_CORRUPT;
    }
    c->first_rec = (i32)im->rec_count;
    i32 acc = 0;
    for (u32 i = 0; i < count; ++i) {
        acc += untransform(refs[i]);
        rec *e = &im->recs[im->rec_count];
        memset(e, 0, sizeof *e);
        e->ref = acc;
        e->cls = (i32)(c - im->classes);
        e->name = 0xFFFFFFFFu;
        e->parent = e->first_child = e->last_child = e->next_sibling = -1;
        e->rot[0] = e->rot[4] = e->rot[8] = 1;
        e->size = v3(4, 1, 2);
        e->color = 0xA3A2A5;
        e->material = 256;
        e->shape = 1;
        e->anchored = 1;
        e->can_collide = 1;
        e->part0 = e->part1 = -1;
        e->joint_on = 1;
        e->source = 0xFFFFFFFFu;
        if (map_get(im, acc) >= 0) {
            plat_free(refs);
            return UB_LOAD_CORRUPT; /* duplicate referent */
        }
        map_put(im, acc, (i32)im->rec_count);
        im->rec_count++;
    }
    plat_free(refs);
    return UB_LOAD_OK;
}

static ub_load_result chunk_prop(importer *im, rd *r)
{
    u32 id = rd_u32(r), pl;
    const u8 *pname = rd_str(r, &pl);
    u32 type = rd_u8(r);
    cls *c = find_class(im, id);
    if (r->bad || !pname) return UB_LOAD_CORRUPT;
    if (!c) return UB_LOAD_OK; /* property for a class we never saw: ignore */
    u32 n = c->count;
    rec *recs = im->recs + c->first_rec;
    bool part = is_part_kind(c->k), light = c->k == K_LIGHTING;

    if (name_is(pname, pl, "Name") && type == 0x01) {
        for (u32 i = 0; i < n; ++i) {
            u32 sl;
            const u8 *s = rd_str(r, &sl);
            if (!s) return UB_LOAD_CORRUPT;
            recs[i].name = add_name(im, s, sl);
        }
        return UB_LOAD_OK;
    }
    bool joint = c->k == K_JOINT || c->k == K_WELD_CONSTRAINT, ws = c->k == K_WORKSPACE;
    bool script = c->k == K_SCRIPT || c->k == K_LOCALSCRIPT || c->k == K_MODULESCRIPT;
    if (script) {
        if (type == 0x01 && name_is(pname, pl, "Source")) {
            for (u32 i = 0; i < n; ++i) {
                u32 sl;
                const u8 *src = rd_str(r, &sl);
                if (!src) return UB_LOAD_CORRUPT;
                recs[i].source = add_text(im, src, sl, false);
                if (recs[i].source == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
            }
        } else if (type == 0x02 && name_is(pname, pl, "Disabled")) {
            if (!need(r, n)) return UB_LOAD_CORRUPT;
            for (u32 i = 0; i < n; ++i) recs[i].disabled = r->p[r->pos + i] != 0;
            r->pos += n;
        } else if (type == 0x12 && name_is(pname, pl, "RunContext")) {
            u32 *tmp = (u32 *)plat_alloc(sizeof(u32) * (n + 1));
            if (!tmp) return UB_LOAD_NO_MEMORY;
            bool okr = rd_interleaved32(r, n, tmp);
            for (u32 i = 0; okr && i < n; ++i) recs[i].run_context = (u8)(tmp[i] <= 2 ? tmp[i] : 0);
            plat_free(tmp);
            if (!okr) return UB_LOAD_CORRUPT;
        }
        return UB_LOAD_OK;
    }
    if (!part && !light && !joint && !ws) return UB_LOAD_OK;

    /* scratch for interleaved arrays: up to three values per instance */
    u32 *tmp = (u32 *)plat_alloc(sizeof(u32) * ((size_t)n * 3 + 1));
    if (!tmp) return UB_LOAD_NO_MEMORY;
    ub_load_result res = UB_LOAD_OK;

    if (part && type == 0x10 && name_is(pname, pl, "CFrame")) {
        for (u32 i = 0; i < n && !r->bad; ++i) {
            u32 sid = rd_u8(r);
            if (sid == 0) {
                if (!need(r, 36)) break;
                for (int k = 0; k < 9; ++k) {
                    u32 v = le32(r->p + r->pos + (u32)k * 4);
                    memcpy(&recs[i].rot[k], &v, 4);
                }
                r->pos += 36;
            } else if (!ub_rbxl_special_rotation(sid, recs[i].rot)) {
                r->bad = true;
            }
        }
        if (!r->bad && rd_interleaved32x3(r, n, tmp)) {
            /* three arrays: all X, then all Y, then all Z */
            for (u32 i = 0; i < n; ++i)
                recs[i].pos = v3(roblox_float(tmp[i]), roblox_float(tmp[n + i]), roblox_float(tmp[2 * n + i]));
        }
    } else if (part && type == 0x0E && (name_is(pname, pl, "size") || name_is(pname, pl, "Size"))) {
        if (rd_interleaved32x3(r, n, tmp))
            for (u32 i = 0; i < n; ++i)
                recs[i].size = v3(roblox_float(tmp[i]), roblox_float(tmp[n + i]), roblox_float(tmp[2 * n + i]));
    } else if (part && type == 0x1A && name_is(pname, pl, "Color3uint8")) {
        if (need(r, n * 3)) {
            const u8 *q = r->p + r->pos;
            for (u32 i = 0; i < n; ++i) {
                recs[i].color = ((u32)q[i] << 16) | ((u32)q[n + i] << 8) | q[2 * n + i];
                recs[i].has_color3 = true;
            }
            r->pos += n * 3;
        }
    } else if (part && type == 0x0B && name_is(pname, pl, "BrickColor")) {
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) recs[i].brick = tmp[i];
    } else if (part && type == 0x12 && name_is(pname, pl, "Material")) {
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) recs[i].material = tmp[i];
    } else if (part && type == 0x12 && (name_is(pname, pl, "shape") || name_is(pname, pl, "Shape"))) {
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) recs[i].shape = tmp[i];
    } else if (part && type == 0x04 && name_is(pname, pl, "Transparency")) {
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) recs[i].transparency = roblox_float(tmp[i]);
    } else if (part && type == 0x02 && (name_is(pname, pl, "Anchored") || name_is(pname, pl, "CanCollide"))) {
        bool anchored = name_is(pname, pl, "Anchored");
        if (need(r, n)) {
            for (u32 i = 0; i < n; ++i) {
                if (anchored) recs[i].anchored = r->p[r->pos + i] != 0;
                else recs[i].can_collide = r->p[r->pos + i] != 0;
            }
            r->pos += n;
        }
    } else if (joint && type == 0x13 &&
               (c->k == K_JOINT ? name_is(pname, pl, "Part0") || name_is(pname, pl, "Part1")
                                : name_is(pname, pl, "Part0Internal") || name_is(pname, pl, "Part1Internal"))) {
        bool first = pname[4] == '0';
        if (rd_interleaved32(r, n, tmp)) {
            i32 acc = 0; /* referents accumulate; one that matches no instance is none */
            for (u32 i = 0; i < n; ++i) {
                acc += untransform(tmp[i]);
                if (first) recs[i].part0 = acc;
                else recs[i].part1 = acc;
            }
        }
    } else if (joint && type == 0x02 && name_is(pname, pl, "Enabled")) {
        if (need(r, n)) {
            for (u32 i = 0; i < n; ++i) recs[i].joint_on = r->p[r->pos + i] != 0;
            r->pos += n;
        }
    } else if (joint && type == 0x03 && c->k == K_WELD_CONSTRAINT && name_is(pname, pl, "State")) {
        /* bit 0 is Enabled (an enabled WeldConstraint saves State = 3) */
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) recs[i].joint_on = (untransform(tmp[i]) & 1) != 0;
    } else if (ws && type == 0x04 && n > 0 &&
               (name_is(pname, pl, "Gravity") || name_is(pname, pl, "FallenPartsDestroyHeight"))) {
        if (rd_interleaved32(r, n, tmp)) {
            f32 v = roblox_float(tmp[0]);
            if (pname[0] == 'G') im->gravity = v;
            else im->fallen_height = v;
        }
    } else if (light && n > 0) {
        /* Lighting is a service: one instance; read its first value */
        if (type == 0x04) {
            if (rd_interleaved32(r, n, tmp)) {
                f32 v = roblox_float(tmp[0]);
                if (name_is(pname, pl, "ClockTime")) im->light.clock_time = v;
                else if (name_is(pname, pl, "GeographicLatitude")) im->light.latitude = v;
                else if (name_is(pname, pl, "Brightness")) im->light.brightness = v;
                else if (name_is(pname, pl, "FogEnd")) im->light.fog_end = v;
            }
        } else if (type == 0x0C) {
            if (rd_interleaved32x3(r, n, tmp)) {
                f32 rr = roblox_float(tmp[0]), gg = roblox_float(tmp[n]), bb = roblox_float(tmp[2 * n]);
                u32 rgb = ((u32)(ub_clampf(rr, 0, 1) * 255.0f + 0.5f) << 16) |
                          ((u32)(ub_clampf(gg, 0, 1) * 255.0f + 0.5f) << 8) | (u32)(ub_clampf(bb, 0, 1) * 255.0f + 0.5f);
                if (name_is(pname, pl, "Ambient")) im->light.ambient = rgb;
                else if (name_is(pname, pl, "OutdoorAmbient")) im->light.outdoor_ambient = rgb;
                else if (name_is(pname, pl, "FogColor")) im->light.fog_color = rgb;
            }
        } else if (type == 0x01 && name_is(pname, pl, "TimeOfDay")) {
            u32 sl;
            const u8 *s = rd_str(r, &sl); /* "HH:MM:SS" */
            if (s && sl >= 5) {
                f32 h = (f32)((s[0] - '0') * 10 + (s[1] - '0')), m = (f32)((s[3] - '0') * 10 + (s[4] - '0'));
                if (h >= 0 && h < 24 && m >= 0 && m < 60) im->light.clock_time = h + m / 60.0f;
            }
        }
    }
    plat_free(tmp);
    if (r->bad) res = UB_LOAD_CORRUPT;
    return res;
}

static ub_load_result chunk_prnt(importer *im, rd *r)
{
    rd_u8(r); /* version */
    u32 n = rd_u32(r);
    if (r->bad || n > MAX_INSTANCES) return UB_LOAD_CORRUPT;
    u32 *a = (u32 *)plat_alloc(sizeof(u32) * (n ? n : 1)), *b = (u32 *)plat_alloc(sizeof(u32) * (n ? n : 1));
    ub_load_result res = UB_LOAD_OK;
    if (!a || !b) res = UB_LOAD_NO_MEMORY;
    else if (!rd_interleaved32(r, n, a) || !rd_interleaved32(r, n, b)) res = UB_LOAD_CORRUPT;
    else {
        i32 ca = 0, pa = 0;
        for (u32 i = 0; i < n; ++i) {
            ca += untransform(a[i]);
            pa += untransform(b[i]);
            i32 child = map_get(im, ca), parent = pa == -1 ? -1 : map_get(im, pa);
            if (child < 0 || im->recs[child].parented) continue; /* unknown or repeated: ignore */
            rec *c = &im->recs[child];
            c->parented = true;
            c->parent = parent;
            if (parent >= 0) {
                rec *p = &im->recs[parent];
                if (p->last_child >= 0) im->recs[p->last_child].next_sibling = child;
                else p->first_child = child;
                p->last_child = child;
            }
        }
    }
    plat_free(a);
    plat_free(b);
    return res;
}

/* --------------------------------------------------------------- build */

/* Roblox (right-handed, +Z towards the viewer) -> Uniblox (left-handed,
 * +Z forward): negate Z. Rotations become S R S, S = diag(1, 1, -1). */
static void convert_part(const rec *e, kind k, ub_part *p)
{
    const f32 *R = e->rot;
    vec3 cx = v3(R[0], R[3], -R[6]);  /* column 0 = (R00, R10, R20) */
    vec3 cy = v3(R[1], R[4], -R[7]);
    vec3 cz = v3(-R[2], -R[5], R[8]);
    vec3 size = e->size;
    p->pos = v3(e->pos.x, e->pos.y, -e->pos.z);
    p->shape = UB_SHAPE_BOX;
    u32 shape = k == K_PART ? e->shape : k == K_WEDGE ? 3 : k == K_CORNER ? 4 : 1;
    if (shape == 0) { /* ball: diameter is the smallest size */
        f32 d = UB_MIN(size.x, UB_MIN(size.y, size.z));
        size = v3(d, d, d);
        p->shape = UB_SHAPE_BALL;
    } else if (shape == 2) {
        /* Roblox cylinders run along local X; ours along Y. Our X, Y, Z =
         * Roblox -Y, X, Z (still a proper rotation); round, so the
         * diameter is the smaller of Roblox's Y and Z sizes. */
        f32 d = UB_MIN(size.y, size.z);
        vec3 nx = v3_scale(cy, -1.0f), ny = cx;
        cx = nx;
        cy = ny;
        size = v3(d, e->size.x, d);
        p->shape = UB_SHAPE_CYLINDER;
    } else if (shape == 3 || shape == 4) {
        /* Roblox wedges rise towards their back (+Z there, -Z after the
         * axis flip); ours rise towards +Z, so turn it 180 degrees. */
        cx = v3_scale(cx, -1.0f);
        cz = v3_scale(cz, -1.0f);
        p->shape = shape == 3 ? UB_SHAPE_WEDGE : UB_SHAPE_CORNER_WEDGE;
    }
    p->rot.x = cx;
    p->rot.y = cy;
    p->rot.z = cz;
    p->size = size;
    bool glow;
    p->color = e->has_color3 ? e->color : brick_to_rgb(e->brick);
    p->material = map_material(e->material, &glow);
    if (k == K_TRUSS) p->material = UB_MAT_METAL;
    f32 t = ub_clampf(e->transparency, 0.0f, 1.0f);
    p->transparency = (u8)(t * 255.0f + 0.5f);
    p->flags = (u8)((e->anchored ? UB_PART_ANCHORED : 0) | (e->can_collide ? UB_PART_CAN_COLLIDE : 0) |
                    (glow ? UB_PART_GLOW : 0) | (k == K_MESH ? UB_PART_PLACEHOLDER : 0));
    if (k == K_NEGATE) p->transparency = 255; /* negative parts are never drawn */
    ub_part_sanitize(p);
}

static ub_load_result build_world(importer *im, ub_world *w, ub_import_stats *st)
{
    i32 *stack = (i32 *)plat_alloc(sizeof(i32) * 2 * (im->rec_count + 1));
    i32 *made = (i32 *)plat_alloc(sizeof(i32) * (im->rec_count + 1));
    u8 *visited = (u8 *)plat_alloc(im->rec_count + 1);
    if (!stack || !made || !visited) {
        plat_free(stack);
        plat_free(made);
        plat_free(visited);
        return UB_LOAD_NO_MEMORY;
    }
    ub_load_result res = UB_LOAD_OK;
    for (u32 i = 0; i < im->rec_count; ++i) made[i] = -1;
    /* A model file (.rbxm) has no Workspace: put what it holds in one, so
     * it is drawn and simulated like a place. */
    bool has_ws = false;
    for (u32 i = 0; i < im->class_count; ++i) has_ws |= im->classes[i].k == K_WORKSPACE && im->classes[i].count > 0;
    i32 top = -1;
    if (!has_ws && (top = ub_world_add(w, -1, UB_CLASS_WORKSPACE, NULL, "Workspace")) < 0) res = UB_LOAD_NO_MEMORY;
    for (u32 root = 0; root < im->rec_count && res == UB_LOAD_OK; ++root) {
        if (im->recs[root].parent >= 0 || visited[root]) continue;
        u32 sp = 0;
        stack[sp++] = (i32)root;
        stack[sp++] = top; /* (record, world parent) pairs */
        while (sp > 0) {
            i32 wparent = stack[--sp];
            i32 ri = stack[--sp];
            if (visited[ri]) continue; /* cycle or duplicate: already made */
            visited[ri] = 1;
            const rec *e = &im->recs[ri];
            const cls *c = &im->classes[e->cls];
            const char *name = e->name != 0xFFFFFFFFu ? im->names + e->name : c->name;
            ub_class wc = c->k == K_WORKSPACE ? UB_CLASS_WORKSPACE : c->k == K_LIGHTING ? UB_CLASS_LIGHTING
                        : c->k == K_FOLDER ? UB_CLASS_FOLDER : c->k == K_MODEL ? UB_CLASS_MODEL
                        : c->k == K_SPAWN ? UB_CLASS_SPAWN : is_part_kind(c->k) ? UB_CLASS_PART : UB_CLASS_OTHER;
            i32 wi;
            if (is_part_kind(c->k)) {
                ub_part p;
                convert_part(e, c->k, &p);
                wi = ub_world_add_part(w, wparent, wc, c->name, name, &p);
                if (p.transparency >= 250) st->invisible++;
                else if (p.flags & UB_PART_PLACEHOLDER) st->placeholders++;
                else st->parts++;
            } else {
                wi = ub_world_add(w, wparent, wc, c->name, name);
            }
            if (wi >= 0 && (c->k == K_SCRIPT || c->k == K_LOCALSCRIPT || c->k == K_MODULESCRIPT)) {
                if (e->source != 0xFFFFFFFFu && !ub_world_set_source(w, wi, im->names + e->source)) wi = -1;
                else if (wi >= 0) {
                    if (e->disabled) w->inst[wi].flags |= UB_INST_DISABLED;
                    if (c->k == K_LOCALSCRIPT || e->run_context == 2) w->inst[wi].flags |= UB_INST_CLIENT;
                    st->scripts++;
                }
            }
            if (c->k == K_TERRAIN) st->terrain++;
            if (wi < 0) {
                res = UB_LOAD_NO_MEMORY;
                break;
            }
            made[ri] = wi;
            /* push children in reverse so they are made in file order */
            u32 kids = 0;
            for (i32 k = e->first_child; k >= 0; k = im->recs[k].next_sibling) kids++;
            u32 base = sp;
            sp += kids * 2;
            u32 j = 0;
            for (i32 k = e->first_child; k >= 0; k = im->recs[k].next_sibling, ++j) {
                stack[base + (kids - 1 - j) * 2] = k;
                stack[base + (kids - 1 - j) * 2 + 1] = wi;
            }
        }
    }
    /* joints become welds between the two parts they name */
    for (u32 i = 0; i < im->rec_count && res == UB_LOAD_OK; ++i) {
        const rec *e = &im->recs[i];
        kind k = im->classes[e->cls].k;
        if (k != K_JOINT && k != K_WELD_CONSTRAINT) continue;
        i32 ra = map_get(im, e->part0), rb = map_get(im, e->part1);
        i32 a = ra >= 0 ? made[ra] : -1, b = rb >= 0 ? made[rb] : -1;
        if (!e->joint_on || a < 0 || b < 0 || a == b || w->inst[a].part < 0 || w->inst[b].part < 0) {
            st->joints_skipped++;
            continue;
        }
        if (!ub_world_add_weld(w, a, b)) res = UB_LOAD_NO_MEMORY;
        else st->welds++;
    }
    plat_free(stack);
    plat_free(made);
    plat_free(visited);
    return res;
}

bool ub_rbxl_detect(const u8 *data, u32 size)
{
    static const u8 sig[14] = { '<', 'r', 'o', 'b', 'l', 'o', 'x', '!', 0x89, 0xFF, 0x0D, 0x0A, 0x1A, 0x0A };
    return size >= 32 && memcmp(data, sig, 14) == 0;
}

ub_load_result ub_rbxl_load(ub_world *w, const u8 *data, u32 size, ub_import_stats *st)
{
    memset(st, 0, sizeof *st);
    if (!ub_rbxl_detect(data, size)) return UB_LOAD_BAD_FORMAT;
    if (data[14] != 0 || data[15] != 0) return UB_LOAD_UNSUPPORTED; /* format version */
    u32 class_count = le32(data + 16), inst_count = le32(data + 20);
    if (inst_count > MAX_INSTANCES || class_count > inst_count + 1) return UB_LOAD_CORRUPT;

    importer im;
    memset(&im, 0, sizeof im);
    im.light = ub_lighting_default();
    im.light.fog_end = 0;
    im.gravity = UB_GRAVITY_DEFAULT;
    im.fallen_height = UB_FALLEN_HEIGHT_DEFAULT;
    im.recs = (rec *)plat_alloc(sizeof(rec) * (inst_count ? inst_count : 1));
    im.rec_cap = inst_count;
    im.map_cap = 16;
    while (im.map_cap < inst_count * 2) im.map_cap *= 2;
    im.map_keys = (i32 *)plat_alloc(sizeof(i32) * im.map_cap);
    im.map_vals = (i32 *)plat_alloc(sizeof(i32) * im.map_cap);
    ub_load_result res = UB_LOAD_OK;
    if (!im.recs || !im.map_keys || !im.map_vals) res = UB_LOAD_NO_MEMORY;
    else for (u32 i = 0; i < im.map_cap; ++i) im.map_keys[i] = -1;

    u32 pos = 32;
    u8 *chunk = NULL;
    bool ended = false;
    while (res == UB_LOAD_OK && !ended) {
        if (size - pos < 16) { res = UB_LOAD_CORRUPT; break; }
        const u8 *h = data + pos;
        u32 comp = le32(h + 4), raw = le32(h + 8);
        pos += 16;
        u32 stored = comp ? comp : raw;
        if (stored > size - pos || raw > MAX_CHUNK) { res = UB_LOAD_CORRUPT; break; }
        const u8 *body = data + pos;
        pos += stored;
        if (memcmp(h, "END\0", 4) == 0) { ended = true; break; }

        bool interesting = memcmp(h, "INST", 4) == 0 || memcmp(h, "PROP", 4) == 0 || memcmp(h, "PRNT", 4) == 0;
        if (!interesting) continue; /* META, SSTR and future chunks */
        const u8 *payload = body;
        if (comp) {
            plat_free(chunk);
            chunk = (u8 *)plat_alloc(raw ? raw : 1);
            if (!chunk) { res = UB_LOAD_NO_MEMORY; break; }
            i32 got;
            if (ub_zstd_is_frame(body, comp)) {
                got = ub_zstd_decompress(body, comp, chunk, raw);
                im.used_zstd = true;
            } else {
                got = ub_lz4_decompress(body, comp, chunk, raw);
                im.used_lz4 = true;
            }
            if (got != (i32)raw) { res = UB_LOAD_CORRUPT; break; }
            payload = chunk;
        }
        rd r = { payload, raw, 0, false };
        if (memcmp(h, "INST", 4) == 0) {
            res = chunk_inst(&im, &r);
        } else if (memcmp(h, "PROP", 4) == 0) {
            res = chunk_prop(&im, &r);
        } else {
            res = chunk_prnt(&im, &r);
        }
    }
    plat_free(chunk);
    if (res == UB_LOAD_OK && !ended) res = UB_LOAD_CORRUPT;

    if (res == UB_LOAD_OK) {
        st->instances = im.rec_count;
        st->classes = im.class_count;
        ub_fmt(st->compression, sizeof st->compression, "%s%s%s", im.used_zstd ? "zstd" : "",
               im.used_zstd && im.used_lz4 ? "+" : "", im.used_lz4 ? "lz4" : (im.used_zstd ? "" : "none"));
        res = build_world(&im, w, st);
        w->lighting = im.light;
        w->lighting.latitude = ub_clampf(w->lighting.latitude, -90.0f, 90.0f);
        w->lighting.clock_time = ub_clampf(w->lighting.clock_time, 0.0f, 24.0f);
        w->lighting.brightness = ub_clampf(w->lighting.brightness, 0.0f, 10.0f);
        if (!(w->lighting.fog_end >= 0.0f) || w->lighting.fog_end > 1.0e5f) w->lighting.fog_end = 0.0f;
        w->gravity = im.gravity >= -1.0e4f && im.gravity <= 1.0e4f ? im.gravity : UB_GRAVITY_DEFAULT;
        w->fallen_height = im.fallen_height >= -1.0e6f && im.fallen_height <= 1.0e6f ? im.fallen_height
                                                                                     : UB_FALLEN_HEIGHT_DEFAULT;
    }
    plat_free(im.classes);
    plat_free(im.recs);
    plat_free(im.map_keys);
    plat_free(im.map_vals);
    plat_free(im.names);
    return res;
}
