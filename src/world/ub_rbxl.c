#include "world/ub_rbxl.h"
#include "world/ub_classes.h"
#include "core/ub_compress.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "render/ub_mesh.h"
#include "world/ub_looks.h"
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
    K_DATA,              /* a class in the class table (GUI, values...): all its properties are kept */
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
    u32 phys_off, phys_len; /* unions, MeshParts: their CSGPHS collision data in the blob arena (0 = none) */
    vec3 init_size;      /* unions, MeshParts: the size their geometry was made at */
} rec;

/* A property of a K_DATA instance, kept until the world is built. */
typedef struct {
    u32 rec;             /* record index */
    u32 name;            /* offset of the property name in the name pool */
    u8 type;             /* ub_ptype */
    u8 from_content;     /* a Content value (2024 files): applied last, if not empty */
    u32 a[4];            /* the value: numbers as bits; strings (offset, length) in the name pool;
                            sequences (offset, keypoints) in seq; fonts (family, weight, style);
                            references the referent */
} iprop;

typedef struct {
    cls *classes;
    u32 class_count, class_cap;
    iprop *props;
    u32 prop_count, prop_cap;
    f32 *seq;
    u32 seq_len, seq_cap;
    rec *recs;
    u32 rec_count, rec_cap;   /* rec_cap = the header's instance count */
    i32 *map_keys, *map_vals;   /* referent -> record index (open addressing) */
    u32 map_cap;
    char *names;
    u32 names_len, names_cap;
    ub_lighting light;
    f32 gravity, fallen_height;
    bool used_zstd, used_lz4;
    u8 *blob;            /* shared strings and binary data kept for building (meshes) */
    u32 blob_len, blob_cap;
    u32 *sstr_off, *sstr_len; /* SSTR: each shared string's place in blob */
    u32 sstr_count;
} importer;

/* Appends bytes to the blob arena; their offset, or 0xFFFFFFFF. */
static u32 add_blob(importer *im, const u8 *p, u32 n)
{
    if (im->blob_len + n + 1 > im->blob_cap) {
        u32 nc = im->blob_cap ? im->blob_cap * 2 : 65536;
        while (nc < im->blob_len + n + 1) nc *= 2;
        u8 *nb = (u8 *)plat_alloc(nc);
        if (!nb) return 0xFFFFFFFFu;
        if (im->blob) {
            memcpy(nb, im->blob, im->blob_len);
            plat_free(im->blob);
        }
        im->blob = nb;
        im->blob_cap = nc;
    }
    u32 off = im->blob_len;
    memcpy(im->blob + off, p, n);
    im->blob_len += n;
    return off;
}

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
    if (c->k == K_OTHER && ub_cdb_class(c->name) >= 0) c->k = K_DATA;
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

/* ------------------------------------------------- properties as data */

static bool grow_buf(void **p, u32 *cap, u32 need, size_t elem)
{
    if (need <= *cap) return true;
    u32 nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void *np = plat_alloc((size_t)nc * elem);
    if (!np) return false;
    if (*p) {
        memcpy(np, *p, (size_t)*cap * elem);
        plat_free(*p);
    }
    *p = np;
    *cap = nc;
    return true;
}

static iprop *new_prop(importer *im, u32 ri, u32 name, u8 type)
{
    if (!grow_buf((void **)&im->props, &im->prop_cap, im->prop_count + 1, sizeof(iprop))) return NULL;
    iprop *p = &im->props[im->prop_count++];
    memset(p, 0, sizeof *p);
    p->rec = ri;
    p->name = name;
    p->type = type;
    return p;
}

static u32 f2u(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static f32 u2f(u32 u)
{
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}

/* Interleaved big-endian 64-bit values, zig-zag transformed (Int64). */
static bool rd_interleaved64(rd *r, u32 count, f64 *out)
{
    if (count > (r->len - r->pos) / 8 || !need(r, count * 8)) {
        r->bad = true;
        return false;
    }
    const u8 *q = r->p + r->pos;
    for (u32 i = 0; i < count; ++i) {
        u64 v = 0;
        for (u32 b = 0; b < 8; ++b) v = (v << 8) | q[b * count + i];
        i64 sv = (i64)((v >> 1) ^ (0u - (v & 1)));
        out[i] = (f64)sv;
    }
    r->pos += count * 8;
    return true;
}

/* Properties Uniblox keeps elsewhere or that mean nothing to it. */
static bool skip_data_prop(const u8 *s, u32 n)
{
    static const char *const skip[] = { "AttributesSerialize", "Tags", "SourceAssetId", "UniqueId", "HistoryId",
                                        "Capabilities", "DefinesCapabilities", "Sandboxed", "SerializedOverrides",
                                        "Archivable", NULL };
    for (int i = 0; skip[i]; ++i)
        if (name_is(s, n, skip[i])) return true;
    return false;
}

static ub_load_result data_prop(importer *im, cls *c, const u8 *pname, u32 pl, u32 type, rd *r)
{
    if (skip_data_prop(pname, pl)) return UB_LOAD_OK;
    u32 n = c->count, base = (u32)c->first_rec;
    if (!n) return UB_LOAD_OK;
    /* The Content type (2024 files) replaced ContentId properties such as
     * Image with ImageContent: keep the address under the old name. */
    u32 keep = pl;
    if (type == 0x22) {
        if (pl <= 7 || !name_is(pname + pl - 7, 7, "Content")) return UB_LOAD_OK;
        keep = pl - 7;
    }
    u32 name = add_name(im, pname, keep);
    if (name == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
    u32 *tmp = (u32 *)plat_alloc(sizeof(u32) * ((size_t)n * 4 + 1));
    if (!tmp) return UB_LOAD_NO_MEMORY;
    ub_load_result res = UB_LOAD_OK;
#define NEWP(i, t) iprop *p = new_prop(im, base + (i), name, (t)); if (!p) { res = UB_LOAD_NO_MEMORY; break; }
    switch (type) {
    case 0x01: /* String */
        for (u32 i = 0; i < n; ++i) {
            u32 sl;
            const u8 *str = rd_str(r, &sl);
            if (!str) { res = UB_LOAD_CORRUPT; break; }
            NEWP(i, UB_PV_STRING);
            p->a[0] = add_text(im, str, sl, false);
            p->a[1] = sl;
            if (p->a[0] == 0xFFFFFFFFu) { res = UB_LOAD_NO_MEMORY; break; }
        }
        break;
    case 0x02: /* Bool */
        if (!need(r, n)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) { NEWP(i, UB_PV_BOOL); p->a[0] = r->p[r->pos + i] != 0; }
        r->pos += n;
        break;
    case 0x03: /* Int32 */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) { NEWP(i, UB_PV_INT); p->a[0] = (u32)untransform(tmp[i]); }
        break;
    case 0x04: /* Float32 */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) { NEWP(i, UB_PV_FLOAT); p->a[0] = f2u(roblox_float(tmp[i])); }
        break;
    case 0x05: /* Float64 */
        if (!need(r, n * 8)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_DOUBLE);
            memcpy(p->a, r->p + r->pos + i * 8, 8);
        }
        r->pos += n * 8;
        break;
    case 0x1B: { /* Int64 */
        f64 *d = (f64 *)plat_alloc(sizeof(f64) * n);
        if (!d) { res = UB_LOAD_NO_MEMORY; break; }
        if (!rd_interleaved64(r, n, d)) res = UB_LOAD_CORRUPT;
        for (u32 i = 0; i < n && res == UB_LOAD_OK; ++i) {
            iprop *p = new_prop(im, base + i, name, UB_PV_DOUBLE);
            if (!p) res = UB_LOAD_NO_MEMORY;
            else memcpy(p->a, &d[i], 8);
        }
        plat_free(d);
        break;
    }
    case 0x06: /* UDim: scales, then offsets */
        if (!rd_interleaved32(r, n, tmp) || !rd_interleaved32(r, n, tmp + n)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_UDIM);
            p->a[0] = f2u(roblox_float(tmp[i]));
            p->a[1] = (u32)untransform(tmp[n + i]);
        }
        break;
    case 0x07: /* UDim2: X.Scale, Y.Scale, X.Offset, Y.Offset */
        if (!rd_interleaved32x3(r, n, tmp) || !rd_interleaved32(r, n, tmp + 3 * n)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_UDIM2);
            p->a[0] = f2u(roblox_float(tmp[i]));
            p->a[1] = (u32)untransform(tmp[2 * n + i]);
            p->a[2] = f2u(roblox_float(tmp[n + i]));
            p->a[3] = (u32)untransform(tmp[3 * n + i]);
        }
        break;
    case 0x0B: /* BrickColor */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) { NEWP(i, UB_PV_BRICKCOLOR); p->a[0] = tmp[i]; }
        break;
    case 0x0C: /* Color3 */
    case 0x0E: /* Vector3 */
        if (!rd_interleaved32x3(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, type == 0x0C ? UB_PV_COLOR3 : UB_PV_VECTOR3);
            for (int k = 0; k < 3; ++k) p->a[k] = f2u(roblox_float(tmp[k * n + i]));
        }
        break;
    case 0x0D: /* Vector2 */
        if (!rd_interleaved32(r, n, tmp) || !rd_interleaved32(r, n, tmp + n)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_VECTOR2);
            p->a[0] = f2u(roblox_float(tmp[i]));
            p->a[1] = f2u(roblox_float(tmp[n + i]));
        }
        break;
    case 0x18: /* Rect: Min.X, Min.Y, Max.X, Max.Y */
        if (!rd_interleaved32x3(r, n, tmp) || !rd_interleaved32(r, n, tmp + 3 * n)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_RECT);
            for (int k = 0; k < 4; ++k) p->a[k] = f2u(roblox_float(tmp[k * n + i]));
        }
        break;
    case 0x1A: /* Color3uint8: R..., G..., B... */
        if (!need(r, n * 3)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_COLOR3);
            for (int k = 0; k < 3; ++k) p->a[k] = f2u(r->p[r->pos + (u32)k * n + i] / 255.0f);
        }
        r->pos += n * 3;
        break;
    case 0x12: /* Enum */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) { NEWP(i, UB_PV_ENUM); p->a[0] = tmp[i]; }
        break;
    case 0x13: { /* Referent: deltas */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        i32 acc = 0;
        for (u32 i = 0; i < n; ++i) {
            acc += untransform(tmp[i]);
            NEWP(i, UB_PV_REF);
            p->a[0] = (u32)acc;
        }
        break;
    }
    case 0x17: /* NumberRange */
        if (!need(r, n * 8)) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n; ++i) {
            NEWP(i, UB_PV_NUMRANGE);
            p->a[0] = le32(r->p + r->pos + i * 8);
            p->a[1] = le32(r->p + r->pos + i * 8 + 4);
        }
        r->pos += n * 8;
        break;
    case 0x15: /* NumberSequence: count, then (time, value, envelope) */
    case 0x16: /* ColorSequence: count, then (time, r, g, b, envelope) */
        for (u32 i = 0; i < n; ++i) {
            u32 kp = rd_u32(r), fields = type == 0x15 ? 3 : 5;
            if (r->bad || kp > 1000 || !need(r, kp * fields * 4)) { res = UB_LOAD_CORRUPT; break; }
            const u8 *q = r->p + r->pos;
            r->pos += kp * fields * 4;
            if (kp < 2 || kp > UB_SEQ_MAX_KEYS) continue; /* not a value Roblox makes: drop it */
            u32 stride = type == 0x15 ? 3 : 4;
            if (!grow_buf((void **)&im->seq, &im->seq_cap, im->seq_len + kp * stride, sizeof(f32))) {
                res = UB_LOAD_NO_MEMORY;
                break;
            }
            NEWP(i, type == 0x15 ? UB_PV_NUMSEQ : UB_PV_COLORSEQ);
            p->a[0] = im->seq_len;
            p->a[1] = kp;
            for (u32 k = 0; k < kp; ++k)
                for (u32 f = 0; f < stride; ++f) im->seq[im->seq_len++] = u2f(le32(q + (k * fields + f) * 4));
        }
        break;
    case 0x20: /* Font: family, weight (u16), style (u8), cached face id */
        for (u32 i = 0; i < n; ++i) {
            u32 fl, cl;
            const u8 *fam = rd_str(r, &fl);
            u32 wlo = rd_u8(r), whi = rd_u8(r), style = rd_u8(r);
            const u8 *cached = rd_str(r, &cl);
            if (!fam || !cached || r->bad) { res = UB_LOAD_CORRUPT; break; }
            NEWP(i, UB_PV_FONT);
            p->a[0] = add_text(im, fam, fl > 1000 ? 1000 : fl, false);
            p->a[1] = wlo | (whi << 8);
            p->a[2] = style;
            if (p->a[0] == 0xFFFFFFFFu) { res = UB_LOAD_NO_MEMORY; break; }
        }
        break;
    case 0x22: { /* Content: source types, then the addresses of those that are URIs (docs/binary.md in rbx-dom) */
        if (!rd_interleaved32(r, n, tmp)) { res = UB_LOAD_CORRUPT; break; }
        u32 uris = rd_u32(r), used = 0;
        if (r->bad || uris > n) { res = UB_LOAD_CORRUPT; break; }
        for (u32 i = 0; i < n && used < uris; ++i) {
            if (untransform(tmp[i]) != 1) continue; /* zig-zag, like Int32: 1 = Uri; 0 None, 2 an object */
            u32 sl;
            const u8 *str = rd_str(r, &sl);
            if (!str) { res = UB_LOAD_CORRUPT; break; }
            used++;
            NEWP(i, UB_PV_STRING);
            p->from_content = 1;
            p->a[0] = add_text(im, str, sl, false);
            p->a[1] = sl;
            if (p->a[0] == 0xFFFFFFFFu) { res = UB_LOAD_NO_MEMORY; break; }
        }
        break; /* object references follow; not used */
    }
    default: break; /* types Uniblox does not keep (CFrame, Faces...) */
    }
#undef NEWP
    plat_free(tmp);
    return res;
}

/* Sets the kept properties on the instances made from the records. */
static ub_load_result apply_props(importer *im, ub_world *w, const i32 *made)
{
    u32 last_name = 0xFFFFFFFFu, key = UB_NO_STRING;
    for (u32 n = 0; n < im->prop_count * 2; ++n) {
        /* first pass: everything else; second: Content values over empty addresses */
        const iprop *p = &im->props[n % im->prop_count];
        if ((n < im->prop_count) == (p->from_content != 0)) continue;
        i32 wi = made[p->rec];
        if (wi < 0) continue;
        if (p->from_content && !im->names[p->a[0]]) continue;
        if (p->name != last_name) {
            key = ub_world_key(w, im->names + p->name);
            last_name = p->name;
            if (key == UB_NO_STRING) return UB_LOAD_NO_MEMORY;
        }
        ub_value v;
        memset(&v, 0, sizeof v);
        v.type = p->type;
        switch (p->type) {
        case UB_PV_STRING:
            v.v.str.s = im->names + p->a[0];
            v.v.str.len = p->a[2] ? p->a[1] : (u32)ub_strlen(v.v.str.s);
            break;
        case UB_PV_BOOL: v.v.b = p->a[0] != 0; break;
        case UB_PV_INT: case UB_PV_ENUM: case UB_PV_BRICKCOLOR: v.v.i = (i32)p->a[0]; break;
        case UB_PV_DOUBLE: /* NaN only where scripts put it: attributes */
            memcpy(&v.v.d, p->a, 8);
            if (!(v.v.d == v.v.d) && im->names[p->name] != '@') v.v.d = 0;
            break;
        case UB_PV_FLOAT: v.v.f = u2f(p->a[0]); break;
        case UB_PV_REF: {
            i32 rr = map_get(im, (i32)p->a[0]);
            v.v.ref = rr >= 0 ? made[rr] : -1;
            break;
        }
        case UB_PV_NUMSEQ: case UB_PV_COLORSEQ:
            v.v.seq.k = im->seq + p->a[0];
            v.v.seq.n = p->a[1];
            break;
        case UB_PV_FONT:
            v.v.font.family = im->names + p->a[0];
            v.v.font.weight = (u16)p->a[1];
            v.v.font.style = (u8)p->a[2];
            break;
        default: /* plain floats: Color3, Vector2/3, UDim, UDim2, Rect, NumberRange */
            memcpy(&v.v, p->a, sizeof p->a);
            break;
        }
        /* NaN never reaches the world (it would poison layout) */
        if (v.type == UB_PV_FLOAT && !(v.v.f == v.v.f)) v.v.f = 0;
        if (v.type == UB_PV_COLOR3 || v.type == UB_PV_VECTOR2 || v.type == UB_PV_VECTOR3 || v.type == UB_PV_RECT ||
            v.type == UB_PV_NUMRANGE) {
            f32 *f = (f32 *)&v.v;
            for (int k = 0; k < 4; ++k) if (!(f[k] == f[k])) f[k] = 0;
        }
        if ((v.type == UB_PV_UDIM || v.type == UB_PV_UDIM2) && !(v.v.ud2.sx == v.v.ud2.sx)) v.v.ud2.sx = 0;
        if (v.type == UB_PV_UDIM2 && !(v.v.ud2.sy == v.v.ud2.sy)) v.v.ud2.sy = 0;
        if (v.type == UB_PV_NUMSEQ || v.type == UB_PV_COLORSEQ)
            for (u32 k = 0; k < v.v.seq.n * (v.type == UB_PV_NUMSEQ ? 3u : 4u); ++k)
                if (!(v.v.seq.k[k] == v.v.seq.k[k])) v.v.seq.k[k] = 0;
        if (!ub_world_set_prop(w, wi, key, &v)) return UB_LOAD_NO_MEMORY;
    }
    return UB_LOAD_OK;
}

/* ------------------------------------------- attributes and tags
 *
 * Any instance may have AttributesSerialize (a blob: u32 count, then per
 * attribute a u32-prefixed name, a type byte and the value; docs/
 * attributes.md in rbx-dom) and Tags (names separated by NULs). Both
 * become typed properties: "@Name" per attribute, and "Tags" as the raw
 * NUL-separated text. CFrame and EnumItem attributes are skipped. */

typedef struct {
    const u8 *p;
    u32 n, pos;
    bool bad;
} ablob;

static bool ab_need(ablob *b, u32 k)
{
    if (b->bad || k > b->n - b->pos) b->bad = true;
    return !b->bad;
}

static u32 ab_u32(ablob *b)
{
    if (!ab_need(b, 4)) return 0;
    u32 v = le32(b->p + b->pos);
    b->pos += 4;
    return v;
}

static u32 ab_u8(ablob *b)
{
    if (!ab_need(b, 1)) return 0;
    return b->p[b->pos++];
}

static const u8 *ab_str(ablob *b, u32 *len)
{
    *len = ab_u32(b);
    if (!ab_need(b, *len)) return NULL;
    const u8 *s = b->p + b->pos;
    b->pos += *len;
    return s;
}

/* A string property kept with its length (it may hold NULs). */
static iprop *text_prop(importer *im, u32 ri, u32 name, const u8 *s, u32 len)
{
    iprop *p = new_prop(im, ri, name, UB_PV_STRING);
    if (!p) return NULL;
    p->a[0] = add_text(im, s, len, true);
    p->a[1] = len;
    p->a[2] = 1; /* use the length, not strlen */
    if (p->a[0] == 0xFFFFFFFFu) return NULL;
    return p;
}

static ub_load_result parse_attributes(importer *im, u32 ri, const u8 *data, u32 len)
{
    ablob b = { data, len, 0, false };
    u32 count = ab_u32(&b);
    for (u32 a = 0; a < count && !b.bad; ++a) {
        u32 kl;
        const u8 *key = ab_str(&b, &kl);
        u32 type = ab_u8(&b);
        if (!key || b.bad) break;
        char kb[202];
        if (kl > 200) kl = 200;
        kb[0] = '@';
        memcpy(kb + 1, key, kl);
        u32 name = add_name(im, (const u8 *)kb, kl + 1);
        if (name == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
        iprop *p = NULL;
        u32 f[9];
        switch (type) {
        case 0x02: { /* String */
            u32 sl;
            const u8 *str = ab_str(&b, &sl);
            if (!str) break;
            if (!text_prop(im, ri, name, str, sl)) return UB_LOAD_NO_MEMORY;
            break;
        }
        case 0x03: /* Bool */
            f[0] = ab_u8(&b);
            if (b.bad) break;
            if (!(p = new_prop(im, ri, name, UB_PV_BOOL))) return UB_LOAD_NO_MEMORY;
            p->a[0] = f[0] != 0;
            break;
        case 0x04: case 0x05: case 0x06: { /* Int32, Float32, Float64: all numbers to scripts */
            f64 d;
            if (type == 0x06) {
                if (!ab_need(&b, 8)) break;
                memcpy(&d, b.p + b.pos, 8);
                b.pos += 8;
            } else {
                u32 u = ab_u32(&b);
                d = type == 0x04 ? (f64)(i32)u : (f64)u2f(u);
            }
            if (b.bad) break;
            if (!(p = new_prop(im, ri, name, UB_PV_DOUBLE))) return UB_LOAD_NO_MEMORY;
            memcpy(p->a, &d, 8);
            break;
        }
        case 0x09: case 0x0A: case 0x0F: case 0x10: case 0x11: case 0x1B: case 0x1C: { /* fixed floats */
            static const struct { u8 id, n, t; } k[] = {
                { 0x09, 2, UB_PV_UDIM }, { 0x0A, 4, UB_PV_UDIM2 }, { 0x0F, 3, UB_PV_COLOR3 },
                { 0x10, 2, UB_PV_VECTOR2 }, { 0x11, 3, UB_PV_VECTOR3 }, { 0x1B, 2, UB_PV_NUMRANGE },
                { 0x1C, 4, UB_PV_RECT },
            };
            int ki = 0;
            while (k[ki].id != type) ki++;
            for (u32 i = 0; i < k[ki].n; ++i) f[i] = ab_u32(&b);
            if (b.bad) break;
            if (!(p = new_prop(im, ri, name, k[ki].t))) return UB_LOAD_NO_MEMORY;
            for (u32 i = 0; i < k[ki].n; ++i) p->a[i] = f[i]; /* UDim offsets are i32 in the same slots */
            break;
        }
        case 0x0E: /* BrickColor */
            f[0] = ab_u32(&b);
            if (b.bad) break;
            if (!(p = new_prop(im, ri, name, UB_PV_BRICKCOLOR))) return UB_LOAD_NO_MEMORY;
            p->a[0] = f[0];
            break;
        case 0x14: /* CFrame: position, rotation id, matrix if the id is 0 */
            for (int i = 0; i < 3; ++i) ab_u32(&b);
            if (ab_u8(&b) == 0)
                for (int i = 0; i < 9; ++i) ab_u32(&b);
            break;
        case 0x15: { /* EnumItem: enum name, value */
            u32 sl;
            ab_str(&b, &sl);
            ab_u32(&b);
            break;
        }
        case 0x17: case 0x19: { /* NumberSequence (envelope, time, value), ColorSequence (envelope, time, r, g, b) */
            u32 kp = ab_u32(&b), fields = type == 0x17 ? 3 : 5;
            if (b.bad || kp > 1000 || !ab_need(&b, kp * fields * 4)) { b.bad = true; break; }
            const u8 *q = b.p + b.pos;
            b.pos += kp * fields * 4;
            if (kp < 2 || kp > UB_SEQ_MAX_KEYS) break;
            u32 stride = type == 0x17 ? 3 : 4;
            if (!grow_buf((void **)&im->seq, &im->seq_cap, im->seq_len + kp * stride, sizeof(f32)))
                return UB_LOAD_NO_MEMORY;
            if (!(p = new_prop(im, ri, name, type == 0x17 ? UB_PV_NUMSEQ : UB_PV_COLORSEQ))) return UB_LOAD_NO_MEMORY;
            p->a[0] = im->seq_len;
            p->a[1] = kp;
            for (u32 i = 0; i < kp; ++i) {
                const u8 *e = q + i * fields * 4;
                f32 env = u2f(le32(e)), t = u2f(le32(e + 4));
                im->seq[im->seq_len++] = t;
                if (type == 0x17) {
                    im->seq[im->seq_len++] = u2f(le32(e + 8));
                    im->seq[im->seq_len++] = env;
                } else
                    for (int c = 0; c < 3; ++c) im->seq[im->seq_len++] = u2f(le32(e + 8 + c * 4));
            }
            break;
        }
        case 0x21: { /* Font: weight u16, style u8, family, cached face */
            u32 wlo = ab_u8(&b), whi = ab_u8(&b), style = ab_u8(&b), fl, cl;
            const u8 *fam = ab_str(&b, &fl);
            ab_str(&b, &cl);
            if (!fam || b.bad) break;
            if (!(p = new_prop(im, ri, name, UB_PV_FONT))) return UB_LOAD_NO_MEMORY;
            p->a[0] = add_text(im, fam, fl > 1000 ? 1000 : fl, false);
            p->a[1] = wlo | (whi << 8);
            p->a[2] = style;
            if (p->a[0] == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
            break;
        }
        default: b.bad = true; break; /* a type this reader does not know: the rest cannot be found */
        }
    }
    return UB_LOAD_OK;
}

static ub_load_result meta_prop(importer *im, cls *c, bool tags, rd *r)
{
    u32 name = 0;
    if (tags) {
        name = add_name(im, (const u8 *)"Tags", 4);
        if (name == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
    }
    for (u32 i = 0; i < c->count; ++i) {
        u32 sl;
        const u8 *s = rd_str(r, &sl);
        if (!s) return UB_LOAD_CORRUPT;
        if (!sl) continue;
        ub_load_result res = UB_LOAD_OK;
        if (tags) {
            if (!text_prop(im, (u32)c->first_rec + i, name, s, sl)) res = UB_LOAD_NO_MEMORY;
        } else
            res = parse_attributes(im, (u32)c->first_rec + i, s, sl);
        if (res != UB_LOAD_OK) return res;
    }
    return UB_LOAD_OK;
}

/* SSTR: version, count, then (16-byte hash, string) each. */
static ub_load_result chunk_sstr(importer *im, rd *r)
{
    rd_u32(r);
    u32 n = rd_u32(r);
    if (r->bad || n > r->len / 20) return UB_LOAD_CORRUPT;
    plat_free(im->sstr_off);
    plat_free(im->sstr_len);
    im->sstr_off = (u32 *)plat_alloc(sizeof(u32) * (n + 1));
    im->sstr_len = (u32 *)plat_alloc(sizeof(u32) * (n + 1));
    if (!im->sstr_off || !im->sstr_len) return UB_LOAD_NO_MEMORY;
    im->sstr_count = n;
    for (u32 i = 0; i < n; ++i) {
        if (!need(r, 16)) return UB_LOAD_CORRUPT;
        r->pos += 16;
        u32 sl;
        const u8 *str = rd_str(r, &sl);
        if (!str) return UB_LOAD_CORRUPT;
        u32 off = add_blob(im, str, sl);
        if (off == 0xFFFFFFFFu) return UB_LOAD_NO_MEMORY;
        im->sstr_off[i] = off;
        im->sstr_len[i] = sl;
    }
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
    if (type == 0x01 && (name_is(pname, pl, "AttributesSerialize") || name_is(pname, pl, "Tags")))
        return meta_prop(im, c, name_is(pname, pl, "Tags"), r);
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
    if (c->k == K_DATA) return data_prop(im, c, pname, pl, type, r);
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
    } else if (c->k == K_MESH && type == 0x1C && name_is(pname, pl, "PhysicalConfigData")) {
        if (rd_interleaved32(r, n, tmp))
            for (u32 i = 0; i < n; ++i) {
                u32 si = tmp[i]; /* big-endian u32s, not transformed */
                if (si < im->sstr_count && im->sstr_len[si] > 10 && !memcmp(im->blob + im->sstr_off[si], "CSGPHS", 6)) {
                    recs[i].phys_off = im->sstr_off[si];
                    recs[i].phys_len = im->sstr_len[si];
                }
            }
    } else if (c->k == K_MESH && type == 0x01 && name_is(pname, pl, "PhysicsData")) {
        for (u32 i = 0; i < n && res == UB_LOAD_OK; ++i) { /* older files: the same data, in place */
            u32 sl;
            const u8 *str = rd_str(r, &sl);
            if (!str) { res = UB_LOAD_CORRUPT; break; }
            if (sl > 10 && !memcmp(str, "CSGPHS", 6) && !recs[i].phys_len) {
                u32 off = add_blob(im, str, sl);
                if (off == 0xFFFFFFFFu) { res = UB_LOAD_NO_MEMORY; break; }
                recs[i].phys_off = off;
                recs[i].phys_len = sl;
            }
        }
    } else if (c->k == K_MESH && ((type == 0x01 && name_is(pname, pl, "MeshId")) ||
                                  (type == 0x22 && name_is(pname, pl, "MeshContent")))) {
        plat_free(tmp); /* kept as a property ("MeshId", or "Mesh" from the newer Content type) */
        return data_prop(im, c, pname, pl, type, r);
    } else if (c->k == K_MESH && type == 0x0E && name_is(pname, pl, "InitialSize")) {
        if (rd_interleaved32x3(r, n, tmp))
            for (u32 i = 0; i < n; ++i)
                recs[i].init_size = v3(roblox_float(tmp[i]), roblox_float(tmp[n + i]), roblox_float(tmp[2 * n + i]));
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

/* ------------------------------------------------------- CSGPHS
 *
 * The collision data Roblox saves with unions and MeshParts (milestone
 * 8C): "CSGPHS", a u32 version (6, or 7 with one more byte), then the
 * solid's volume, centre and inertia (10 floats), then convex pieces to
 * the end, each: a 40-byte header, u32 float count, u32 4, the vertices
 * (x y z floats), u32 index count, the triangle indices (u32). Vertices
 * are in the part's space at InitialSize. Worked out from rbx-test-files'
 * unions; read defensively. */
static u16 csg_mesh(ub_world *w, const u8 *d, u32 len, vec3 init)
{
    if (len < 10 + 40 || memcmp(d, "CSGPHS", 6) != 0) return 0;
    u32 ver = le32(d + 6);
    u32 p = ver == 7 ? 11 : 10;
    if (ver != 6 && ver != 7) return 0;
    p += 40;
    /* first pass: count, and the bounds (for a missing InitialSize) */
    u32 nv = 0, nt = 0, np = 0;
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (u32 q = p; q < len;) {
        if (len - q < 48 || le32(d + q) != 16) return 0;
        q += 40;
        u32 nf = le32(d + q), fs = le32(d + q + 4);
        q += 8;
        if (fs != 4 || nf % 3 || nf > (len - q) / 4) return 0;
        for (u32 i = 0; i < nf; ++i) {
            f32 f = u2f(le32(d + q + i * 4));
            if (!(f > -1e5f && f < 1e5f)) return 0;
            f32 *lo_c = &lo.x + i % 3, *hi_c = &hi.x + i % 3;
            if (f < *lo_c) *lo_c = f;
            if (f > *hi_c) *hi_c = f;
        }
        q += nf * 4;
        if (len - q < 4) return 0;
        u32 ni = le32(d + q);
        q += 4;
        if (ni % 3 || ni > (len - q) / 4) return 0;
        for (u32 i = 0; i < ni; ++i)
            if (le32(d + q + i * 4) >= nf / 3) return 0;
        q += ni * 4;
        nv += nf / 3;
        nt += ni / 3;
        np++;
    }
    if (!np || !nt || nv > 0xFFFF) return 0;
    vec3 size = init;
    if (!(size.x > 1e-4f && size.y > 1e-4f && size.z > 1e-4f)) size = v3_sub(hi, lo);
    if (!(size.x > 1e-4f && size.y > 1e-4f && size.z > 1e-4f)) return 0;
    f32 *v = (f32 *)plat_alloc(sizeof(f32) * nv * 3 + sizeof(u32) * (nt * 3 + np * 2));
    if (!v) return 0;
    u32 *tri = (u32 *)(v + nv * 3), *pv = tri + nt * 3, *pt = pv + np;
    u32 vi = 0, ti = 0, pi = 0;
    for (u32 q = p; q < len;) {
        q += 40;
        u32 nf = le32(d + q);
        q += 8;
        u32 base = vi;
        for (u32 i = 0; i < nf / 3; ++i, ++vi) { /* to the unit cube, and Uniblox's axes (z flipped) */
            v[vi * 3] = u2f(le32(d + q + i * 12)) / size.x;
            v[vi * 3 + 1] = u2f(le32(d + q + i * 12 + 4)) / size.y;
            v[vi * 3 + 2] = -u2f(le32(d + q + i * 12 + 8)) / size.z;
        }
        q += nf * 4;
        u32 ni = le32(d + q);
        q += 4;
        for (u32 i = 0; i < ni / 3; ++i, ++ti) {
            tri[ti * 3] = base + le32(d + q + i * 12);
            tri[ti * 3 + 1] = base + le32(d + q + i * 12 + 4);
            tri[ti * 3 + 2] = base + le32(d + q + i * 12 + 8);
        }
        q += ni * 4;
        pv[pi] = vi;
        pt[pi] = ti;
        pi++;
    }
    u16 id = ub_world_add_mesh(w, v, nv, tri, nt, pv, pt, np, 0);
    plat_free(v);
    return id;
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
                memset(&p, 0, sizeof p);
                convert_part(e, c->k, &p);
                if (c->k == K_MESH && e->phys_len) {
                    p.mesh = csg_mesh(w, im->blob + e->phys_off, e->phys_len, e->init_size);
                    if (p.mesh) {
                        p.flags &= (u8)~UB_PART_PLACEHOLDER;
                        st->meshes++;
                    }
                }
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
    /* the properties of GUI objects, values... then whatever older files lack */
    if (res == UB_LOAD_OK) res = apply_props(im, w, made);
    for (u32 i = 0; i < im->rec_count && res == UB_LOAD_OK; ++i) {
        const cls *c = &im->classes[im->recs[i].cls];
        if (c->k != K_DATA || made[i] < 0) continue;
        if (!ub_cdb_fill_defaults(w, made[i], c->name)) res = UB_LOAD_NO_MEMORY;
        if (ub_cdb_is_a(c->name, "GuiBase2d")) st->gui++;
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

        bool interesting = memcmp(h, "INST", 4) == 0 || memcmp(h, "PROP", 4) == 0 || memcmp(h, "PRNT", 4) == 0 ||
                           memcmp(h, "SSTR", 4) == 0;
        if (!interesting) continue; /* META and future chunks */
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
        } else if (memcmp(h, "SSTR", 4) == 0) {
            res = chunk_sstr(&im, &r);
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
        if (res == UB_LOAD_OK) st->looks = (u32)ub_world_resolve_looks(w);
        w->lighting = im.light;
        w->lighting.latitude = ub_clampf(w->lighting.latitude, -90.0f, 90.0f);
        w->lighting.clock_time = ub_clampf(w->lighting.clock_time, 0.0f, 24.0f);
        w->lighting.brightness = ub_clampf(w->lighting.brightness, 0.0f, 10.0f);
        if (!(w->lighting.fog_end >= 0.0f) || w->lighting.fog_end > 1.0e5f) w->lighting.fog_end = 0.0f;
        w->gravity = im.gravity >= -1.0e4f && im.gravity <= 1.0e4f ? im.gravity : UB_GRAVITY_DEFAULT;
        w->fallen_height = im.fallen_height >= -1.0e6f && im.fallen_height <= 1.0e6f ? im.fallen_height
                                                                                     : UB_FALLEN_HEIGHT_DEFAULT;
    }
    plat_free(im.blob);
    plat_free(im.sstr_off);
    plat_free(im.sstr_len);
    plat_free(im.classes);
    plat_free(im.recs);
    plat_free(im.map_keys);
    plat_free(im.map_vals);
    plat_free(im.names);
    plat_free(im.props);
    plat_free(im.seq);
    return res;
}
