/* Uniblox - tests for the world model, .ubx files and the .rbxl importer. */
#include <math.h>
#include <stdlib.h>

#include "physics/ub_physics.h"
#include "platform/platform.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "world/ub_query.h"
#include "world/ub_rbxl.h"
#include "world/ub_classes.h"
#include "world/ub_world.h"
#include "test.h"

static u8 *load_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (u32)n;
    return buf;
}

/* ------------------------------------------------- CFrame special cases */

/* From rbx-test-files' cframe-special-cases XML (row-major R00..R22). */
static const struct { u32 id; i8 m[9]; } k_special[24] = {
    { 0x02, { 1, 0, 0, 0, 1, 0, 0, 0, 1 } },   { 0x03, { 1, 0, 0, 0, 0, -1, 0, 1, 0 } },
    { 0x05, { 1, 0, 0, 0, -1, 0, 0, 0, -1 } }, { 0x06, { 1, 0, 0, 0, 0, 1, 0, -1, 0 } },
    { 0x07, { 0, 1, 0, 1, 0, 0, 0, 0, -1 } },  { 0x09, { 0, 0, 1, 1, 0, 0, 0, 1, 0 } },
    { 0x0a, { 0, -1, 0, 1, 0, 0, 0, 0, 1 } },  { 0x0c, { 0, 0, -1, 1, 0, 0, 0, -1, 0 } },
    { 0x0d, { 0, 1, 0, 0, 0, 1, 1, 0, 0 } },   { 0x0e, { 0, 0, -1, 0, 1, 0, 1, 0, 0 } },
    { 0x10, { 0, -1, 0, 0, 0, -1, 1, 0, 0 } }, { 0x11, { 0, 0, 1, 0, -1, 0, 1, 0, 0 } },
    { 0x14, { -1, 0, 0, 0, 1, 0, 0, 0, -1 } }, { 0x15, { -1, 0, 0, 0, 0, 1, 0, 1, 0 } },
    { 0x17, { -1, 0, 0, 0, -1, 0, 0, 0, 1 } }, { 0x18, { -1, 0, 0, 0, 0, -1, 0, -1, 0 } },
    { 0x19, { 0, 1, 0, -1, 0, 0, 0, 0, 1 } },  { 0x1b, { 0, 0, -1, -1, 0, 0, 0, 1, 0 } },
    { 0x1c, { 0, -1, 0, -1, 0, 0, 0, 0, -1 } }, { 0x1e, { 0, 0, 1, -1, 0, 0, 0, -1, 0 } },
    { 0x1f, { 0, 1, 0, 0, 0, -1, -1, 0, 0 } }, { 0x20, { 0, 0, 1, 0, 1, 0, -1, 0, 0 } },
    { 0x22, { 0, -1, 0, 0, 0, 1, -1, 0, 0 } }, { 0x23, { 0, 0, -1, 0, -1, 0, -1, 0, 0 } },
};

static void test_special_rotations(void)
{
    int good = 0;
    for (int i = 0; i < 24; ++i) {
        f32 m[9];
        bool ok = ub_rbxl_special_rotation(k_special[i].id, m);
        for (int k = 0; k < 9 && ok; ++k) ok = m[k] == (f32)k_special[i].m[k];
        good += ok;
    }
    CHECK(good == 24);
    f32 m[9];
    int invalid = 0; /* the other 12 IDs in 0x02..0x23 are parallel-axis pairs */
    for (u32 id = 0; id < 0x30; ++id) invalid += !ub_rbxl_special_rotation(id, m);
    CHECK(invalid == 0x30 - 24);
}

/* ------------------------------------------------------------ ubx files */

static ub_part make_part(f32 x, f32 y, f32 z, u8 shape, u8 mat, u32 color)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = v3(x, y, z);
    p.rot = mat3_from_euler(x * 0.1f, y * 0.05f, z * 0.02f);
    p.size = v3(1 + x * 0.5f, 2, 3);
    p.color = color;
    p.shape = shape;
    p.material = mat;
    p.transparency = (u8)(x * 10);
    p.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    return p;
}

static void build_sample(ub_world *w)
{
    ub_world_init(w);
    snprintf(w->name, sizeof w->name, "Sample place");
    i32 ws = ub_world_workspace(w);
    ub_world_add(w, -1, UB_CLASS_LIGHTING, NULL, "Lighting");
    i32 model = ub_world_add(w, ws, UB_CLASS_MODEL, NULL, "House");
    for (int i = 0; i < 5; ++i) {
        ub_part p = make_part((f32)i, (f32)(i * 2), (f32)-i, (u8)(i % UB_SHAPE_COUNT), (u8)(i % UB_MAT_COUNT), 0x102030u * (u32)i);
        char name[16];
        snprintf(name, sizeof name, "Wall%d", i);
        ub_world_add_part(w, i < 3 ? model : ws, UB_CLASS_PART, NULL, name, &p);
    }
    i32 folder = ub_world_add(w, model, UB_CLASS_FOLDER, NULL, "Bits");
    ub_world_add(w, folder, UB_CLASS_OTHER, "Script", "Door script");
    ub_part spawn = make_part(0, 1, 0, UB_SHAPE_BOX, UB_MAT_PLASTIC, 0xA3A2A5);
    ub_world_add_part(w, ws, UB_CLASS_SPAWN, NULL, "SpawnLocation", &spawn);
    w->lighting.clock_time = 9.5f;
    w->lighting.fog_end = 500.0f;
    w->gravity = 100.0f;
    w->fallen_height = -250.0f;
    CHECK(ub_world_add_weld(w, ub_world_find_child(w, model, "Wall0"), ub_world_find_child(w, model, "Wall1")));
    CHECK(!ub_world_add_weld(w, model, ub_world_find_child(w, model, "Wall1"))); /* a model is not a part */
}

static void test_ubx(void)
{
    ub_world a, b, c;
    build_sample(&a);
    u8 *d1, *d2;
    u32 n1, n2;
    CHECK(ub_ubx_save(&a, &d1, &n1));
    ub_world_init(&b);
    CHECK(ub_ubx_load(&b, d1, n1) == UB_LOAD_OK);
    CHECK(ub_ubx_save(&b, &d2, &n2));
    CHECK(n1 == n2 && memcmp(d1, d2, n1) == 0);   /* byte-for-byte round trip */
    CHECK(b.count == a.count && b.part_count == a.part_count);
    CHECK(strcmp(b.name, "Sample place") == 0 && b.lighting.clock_time == 9.5f && b.lighting.fog_end == 500.0f);
    i32 house = ub_world_find_child(&b, b.workspace, "House");
    CHECK(house >= 0 && b.inst[house].cls == UB_CLASS_MODEL);
    i32 w2 = ub_world_find_child(&b, house, "Wall2");
    CHECK(w2 >= 0 && b.inst[w2].part >= 0);
    if (w2 >= 0 && b.inst[w2].part >= 0) {
        const ub_part *p = &b.parts[b.inst[w2].part];
        ub_part e = make_part(2, 4, -2, 2, 2, 0x204060);
        CHECK(memcmp(p, &e, sizeof e) == 0);
    }
    i32 script = ub_world_find_child(&b, ub_world_find_child(&b, house, "Bits"), "Door script");
    CHECK(script >= 0 && strcmp(ub_world_str(&b, b.inst[script].class_name), "Script") == 0);
    CHECK(b.gravity == 100.0f && b.fallen_height == -250.0f && b.weld_count == 1);
    if (b.weld_count == 1) {
        CHECK(b.welds[0].a == ub_world_find_child(&b, house, "Wall0"));
        CHECK(b.welds[0].b == ub_world_find_child(&b, house, "Wall1"));
    }
    CHECK(ub_world_is_descendant(&b, script, b.workspace) && !ub_world_is_descendant(&b, b.workspace, script));

    /* every truncation fails cleanly; random corruption never crashes */
    int truncated_ok = 0;
    for (u32 len = 0; len < n1; ++len) {
        ub_world_init(&c);
        truncated_ok += ub_ubx_load(&c, d1, len) != UB_LOAD_OK;
        ub_world_free(&c);
    }
    CHECK(truncated_ok == (int)n1);
    u8 *bad = (u8 *)malloc(n1);
    u32 seed = 99;
    for (int t = 0; t < 3000; ++t) {
        memcpy(bad, d1, n1);
        for (int k = 0; k < 1 + t % 5; ++k) {
            seed = seed * 1103515245u + 12345u;
            bad[(seed >> 8) % n1] ^= (u8)(seed >> 3);
        }
        ub_world_init(&c);
        if (ub_ubx_load(&c, bad, n1) == UB_LOAD_OK) {
            /* whatever loads must be a consistent tree */
            for (i32 i = 0; i < c.count; ++i) CHECK(c.inst[i].parent < i);
            for (i32 i = 0; i < c.weld_count; ++i)
                CHECK(c.inst[c.welds[i].a].part >= 0 && c.inst[c.welds[i].b].part >= 0);
        }
        ub_world_free(&c);
    }
    free(bad);
    CHECK(ub_ubx_load(&c, (const u8 *)"NOTAPLACE...", 12) == UB_LOAD_BAD_FORMAT);
    free(d1);
    free(d2);
    ub_world_free(&a);
    ub_world_free(&b);
}

/* ------------------------------------------------------------- rbxl */

typedef struct {
    char cls[32], name[64];
    f32 pos[3], size[3], rot[9], transparency;
    long color;
    int material, shape;
} expected_part;

static int read_expected(const char *path, expected_part *out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        expected_part *e = &out[n];
        char *fields[9];
        int k = 0;
        fields[k++] = line;
        for (char *p = line; *p && k < 9; ++p)
            if (*p == '|') { *p = 0; fields[k++] = p + 1; }
        if (k < 9) continue;
        snprintf(e->cls, sizeof e->cls, "%s", fields[0]);
        snprintf(e->name, sizeof e->name, "%s", fields[1]);
        sscanf(fields[2], "%f %f %f", &e->pos[0], &e->pos[1], &e->pos[2]);
        sscanf(fields[3], "%f %f %f", &e->size[0], &e->size[1], &e->size[2]);
        e->color = strtol(fields[4], NULL, 10);
        e->material = atoi(fields[5]);
        e->shape = atoi(fields[6]);
        sscanf(fields[7], "%f %f %f %f %f %f %f %f %f", &e->rot[0], &e->rot[1], &e->rot[2], &e->rot[3],
               &e->rot[4], &e->rot[5], &e->rot[6], &e->rot[7], &e->rot[8]);
        e->transparency = (f32)atof(fields[8]);
        n++;
    }
    fclose(f);
    return n;
}

static bool close3(const f32 *a, vec3 b, f32 tol)
{
    return fabsf(a[0] - b.x) < tol && fabsf(a[1] - b.y) < tol && fabsf(a[2] - b.z) < tol;
}

/* Compare one imported file with its XML-derived expectations. */
static void check_rbxl(const char *name, u32 min_instances, bool want_zstd)
{
    char path[256];
    snprintf(path, sizeof path, "tests/data/rbx/%s.rbxl", name);
    u32 n;
    u8 *data = load_file(path, &n);
    if (!data) {
        snprintf(path, sizeof path, "tests/data/rbx/%s.rbxm", name);
        data = load_file(path, &n);
    }
    CHECK(data != NULL);
    if (!data) return;
    ub_world w;
    ub_world_init(&w);
    ub_import_stats st;
    ub_load_result r = ub_rbxl_load(&w, data, n, &st);
    printf("    %s: %s, %u instances, %u parts, %u placeholders, %s\n", name, ub_load_result_name(r),
           st.instances, st.parts, st.placeholders, st.compression);
    CHECK(r == UB_LOAD_OK && st.instances >= min_instances);
    CHECK(want_zstd == (strstr(st.compression, "zstd") != NULL));

    snprintf(path, sizeof path, "tests/data/rbx/%s.expected", name);
    static expected_part ex[64];
    int ne = read_expected(path, ex, 64);
    CHECK(ne > 0);
    int matched = 0;
    for (int i = 0; i < ne; ++i) {
        const expected_part *e = &ex[i];
        /* find the imported instance with this class, name and position
         * (names repeat, e.g. four "Tower" parts) */
        i32 found = -1;
        for (i32 k = 0; k < w.count && found < 0; ++k) {
            const ub_instance *in = &w.inst[k];
            if (in->part < 0 || strcmp(ub_world_str(&w, in->class_name), e->cls) != 0 ||
                strcmp(ub_world_str(&w, in->name), e->name) != 0)
                continue;
            const ub_part *q = &w.parts[in->part];
            if (close3(e->pos, v3(q->pos.x, q->pos.y, -q->pos.z), 1e-3f)) found = k;
        }
        if (found < 0) printf("    not found: %s \"%s\"\n", e->cls, e->name);
        for (i32 k = found; k >= 0 && k < w.count; ++k) {
            const ub_instance *in = &w.inst[k];
            const ub_part *p = &w.parts[in->part];
            /* back to Roblox's axes: negate Z */
            bool ok = close3(e->pos, v3(p->pos.x, p->pos.y, -p->pos.z), 1e-3f);
            if (p->shape == UB_SHAPE_BOX) {
                ok = ok && close3(e->size, p->size, 1e-3f);
                /* R = S R' S; columns of R' are p->rot.x/y/z */
                f32 R[9] = { p->rot.x.x, p->rot.y.x, -p->rot.z.x, p->rot.x.y, p->rot.y.y, -p->rot.z.y,
                             -p->rot.x.z, -p->rot.y.z, p->rot.z.z };
                for (int j = 0; j < 9; ++j) ok = ok && fabsf(R[j] - e->rot[j]) < 1e-4f;
            }
            if (e->color >= 0) ok = ok && p->color == (u32)e->color;
            ok = ok && fabsf((f32)p->transparency / 255.0f - e->transparency) < 0.01f;
            if (!ok)
                printf("    mismatch: %s \"%s\": pos %g %g %g (want %g %g %g) size %g %g %g (want %g %g %g) "
                       "color %06x (want %06lx) transp %u rot0 %g %g %g\n",
                       e->cls, e->name, p->pos.x, p->pos.y, -p->pos.z, e->pos[0], e->pos[1], e->pos[2],
                       p->size.x, p->size.y, p->size.z, e->size[0], e->size[1], e->size[2], p->color, e->color,
                       p->transparency, p->rot.x.x, p->rot.x.y, p->rot.x.z);
            matched += ok;
            break;
        }
    }
    CHECK(matched == ne);
    printf("    %d/%d parts match the expected values\n", matched, ne);

    /* fuzz: corrupted copies must never crash or overrun */
    u8 *bad = (u8 *)malloc(n);
    u32 seed = 4242;
    for (int t = 0; t < 400; ++t) {
        memcpy(bad, data, n);
        for (int k = 0; k < 1 + t % 6; ++k) {
            seed = seed * 1103515245u + 12345u;
            bad[32 + (seed >> 8) % (n - 32)] ^= (u8)(1u << ((seed >> 4) & 7));
        }
        seed = seed * 1103515245u + 12345u;
        u32 len = (t % 3 == 0) ? (seed >> 8) % (n + 1) : n;
        ub_world fw;
        ub_world_init(&fw);
        ub_import_stats fs;
        ub_rbxl_load(&fw, bad, len, &fs);
        ub_world_free(&fw);
    }
    free(bad);
    free(data);
    ub_world_free(&w);
}

static void test_rbxl(void)
{
    check_rbxl("three-unique-parts", 3, false);
    check_rbxl("all-instances-415", 100, false);
    check_rbxl("baseplate-727", 10, true);
    check_rbxl("castle-generated", 20, true);

    /* the generated castle: shapes, glow, invisibility, placeholders, lighting */
    {
        u32 cn;
        u8 *cd = load_file("tests/data/rbx/castle-generated.rbxl", &cn);
        CHECK(cd != NULL);
        if (cd) {
            ub_world w;
            ub_world_init(&w);
            ub_import_stats st;
            CHECK(ub_rbxl_load(&w, cd, cn, &st) == UB_LOAD_OK);
            int cyl = 0, balls = 0, glow = 0, hidden = 0, mesh = 0, upright = 0;
            for (i32 k = 0; k < w.count; ++k) {
                if (w.inst[k].part < 0) continue;
                const ub_part *p = &w.parts[w.inst[k].part];
                if (p->shape == UB_SHAPE_CYLINDER) {
                    cyl++;
                    /* towers are 14 long on Roblox X, turned upright: our Y axis must be vertical */
                    upright += fabsf(p->rot.y.y) > 0.999f && fabsf(p->size.y - 14.0f) < 1e-3f && fabsf(p->size.x - 8.0f) < 1e-3f;
                }
                balls += p->shape == UB_SHAPE_BALL;
                glow += (p->flags & UB_PART_GLOW) != 0;
                hidden += p->transparency == 255;
                mesh += (p->flags & UB_PART_PLACEHOLDER) != 0;
            }
            CHECK(cyl == 4 && upright == 4 && balls == 4 && glow == 1 && hidden == 1 && mesh == 1);
            CHECK(fabsf(w.lighting.clock_time - 17.5f) < 1e-4f);
            CHECK(strcmp(st.compression, "zstd+lz4") == 0);
            ub_world_free(&w);
            free(cd);
        }
    }

    /* the all-instances place: one of each part class, mapped to shapes */
    u32 n;
    u8 *data = load_file("tests/data/rbx/all-instances-415.rbxl", &n);
    if (data) {
        ub_world w;
        ub_world_init(&w);
        ub_import_stats st;
        CHECK(ub_rbxl_load(&w, data, n, &st) == UB_LOAD_OK);
        int wedges = 0, corners = 0, placeholders = 0, spawns = 0;
        for (i32 k = 0; k < w.count; ++k) {
            const ub_instance *in = &w.inst[k];
            if (in->part < 0) continue;
            const ub_part *p = &w.parts[in->part];
            wedges += p->shape == UB_SHAPE_WEDGE;
            corners += p->shape == UB_SHAPE_CORNER_WEDGE;
            placeholders += (p->flags & UB_PART_PLACEHOLDER) != 0;
            spawns += in->cls == UB_CLASS_SPAWN;
        }
        CHECK(wedges == 1 && corners == 1 && placeholders >= 2 && spawns == 1);
        CHECK(w.workspace >= 0);
        /* its joints are all unattached (Part0/Part1 empty) */
        CHECK(st.welds == 0 && st.joints_skipped >= 6 && w.weld_count == 0);
        CHECK(fabsf(w.gravity - 196.2f) < 1e-3f && w.fallen_height == -500.0f);
        /* round trip the imported world through .ubx */
        u8 *a, *b;
        u32 na, nb;
        CHECK(ub_ubx_save(&w, &a, &na));
        ub_world w2;
        ub_world_init(&w2);
        CHECK(ub_ubx_load(&w2, a, na) == UB_LOAD_OK);
        CHECK(ub_ubx_save(&w2, &b, &nb) && na == nb && memcmp(a, b, na) == 0);
        free(a);
        free(b);
        ub_world_free(&w2);
        ub_world_free(&w);
        free(data);
    }

    /* a WeldConstraint joining parts A and B */
    data = load_file("tests/data/rbx/weldconstraint.rbxm", &n);
    CHECK(data != NULL);
    if (data) {
        ub_world w;
        ub_world_init(&w);
        ub_import_stats st;
        CHECK(ub_rbxl_load(&w, data, n, &st) == UB_LOAD_OK);
        CHECK(st.welds == 1 && w.weld_count == 1);
        if (w.weld_count == 1) {
            const char *a = ub_world_str(&w, w.inst[w.welds[0].a].name), *b = ub_world_str(&w, w.inst[w.welds[0].b].name);
            CHECK(strcmp(a, "A") == 0 && strcmp(b, "B") == 0);
            CHECK(!(w.parts[w.inst[w.welds[0].a].part].flags & UB_PART_ANCHORED));
        }
        /* physics makes the welded pair one loose body */
        ub_physics ph;
        CHECK(ub_phys_init(&ph, &w, NULL, 0));
        if (w.weld_count == 1) {
            i32 ba = ub_phys_part_body(&ph, w.inst[w.welds[0].a].part), bb = ub_phys_part_body(&ph, w.inst[w.welds[0].b].part);
            CHECK(ba > 0 && ba == bb && ph.bodies[ba].type == UB_BODY_DYNAMIC && ph.stats.welds == 1);
        }
        ub_phys_free(&ph);
        ub_world_free(&w);
        free(data);
    }

    u8 junk[64] = "<roblox!\x89\xff\x0d\x0a\x1a\x0a";
    ub_world w;
    ub_world_init(&w);
    ub_import_stats st;
    CHECK(ub_rbxl_load(&w, junk, sizeof junk, &st) != UB_LOAD_OK);
    CHECK(ub_rbxl_load(&w, (const u8 *)"hello", 5, &st) == UB_LOAD_BAD_FORMAT);
    ub_world_free(&w);
}

/* ------------------------------------------------------------ raycasts */

static ub_part qpart(ub_shape shape, vec3 pos, vec3 size, mat3 rot)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = pos;
    p.size = size;
    p.rot = rot;
    p.shape = (u8)shape;
    p.flags = UB_PART_CAN_COLLIDE;
    return p;
}

static void test_raycast(void)
{
    ub_meshes_init();
    vec3 down = v3(0, -1, 0);
    f32 t;
    /* box top at y = 1 */
    ub_part box = qpart(UB_SHAPE_BOX, v3(0, 0.5f, 0), v3(4, 1, 4), mat3_identity());
    CHECK(ub_part_raycast(&box, v3(1, 10, 1), down, 100, &t) && fabsf(t - 9.0f) < 1e-4f);
    CHECK(!ub_part_raycast(&box, v3(3, 10, 0), down, 100, &t));        /* beside it */
    CHECK(!ub_part_raycast(&box, v3(0, 10, 0), down, 5, &t));          /* too far */
    CHECK(ub_part_raycast(&box, v3(0, 0.5f, 0), down, 5, &t) && t == 0.0f); /* starting inside */
    /* wedge 4 high, 8 deep, rising towards +Z: surface height = 2 + z/2 relative to centre */
    ub_part wedge = qpart(UB_SHAPE_WEDGE, v3(0, 2, 0), v3(4, 4, 8), mat3_identity());
    for (int i = -3; i <= 3; ++i) {
        f32 z = (f32)i;
        bool hit = ub_part_raycast(&wedge, v3(0, 20, z), down, 100, &t);
        f32 want = 2.0f + z * 0.5f; /* top of the slope above the bottom at y=0 */
        CHECK(hit && fabsf((20.0f - t) - want) < 1e-3f);
    }
    /* turned 90 degrees, the slope rises towards +X instead */
    ub_part wedge90 = qpart(UB_SHAPE_WEDGE, v3(0, 2, 0), v3(4, 4, 8), mat3_from_euler(UB_HALF_PI, 0, 0));
    CHECK(ub_part_raycast(&wedge90, v3(3, 20, 0), down, 100, &t) && fabsf((20.0f - t) - 3.5f) < 1e-3f);
    CHECK(ub_part_raycast(&wedge90, v3(-3, 20, 0), down, 100, &t) && fabsf((20.0f - t) - 0.5f) < 1e-3f);
    /* cylinder (12-sided query mesh): flat top, missed outside its radius */
    ub_part cyl = qpart(UB_SHAPE_CYLINDER, v3(0, 3, 0), v3(2, 6, 2), mat3_identity());
    CHECK(ub_part_raycast(&cyl, v3(0.5f, 10, 0), down, 100, &t) && fabsf(t - 4.0f) < 1e-3f);
    CHECK(!ub_part_raycast(&cyl, v3(0.9f, 10, 0.9f), down, 100, &t));
    /* sideways ray into a box face */
    CHECK(ub_part_raycast(&box, v3(-10, 0.5f, 0), v3(1, 0, 0), 100, &t) && fabsf(t - 8.0f) < 1e-4f);

    /* world query: nearest of several, only collidable parts */
    ub_world w;
    ub_world_init(&w);
    i32 ws = ub_world_workspace(&w);
    ub_part floor = qpart(UB_SHAPE_BOX, v3(0, -0.5f, 0), v3(100, 1, 100), mat3_identity());
    ub_part shelf = qpart(UB_SHAPE_BOX, v3(0, 5, 0), v3(4, 1, 4), mat3_identity());
    ub_part ghost = qpart(UB_SHAPE_BOX, v3(0, 8, 0), v3(4, 1, 4), mat3_identity());
    ghost.flags = 0; /* CanCollide off */
    ub_world_add_part(&w, ws, UB_CLASS_PART, NULL, "Floor", &floor);
    i32 shelf_i = ub_world_add_part(&w, ws, UB_CLASS_PART, NULL, "Shelf", &shelf);
    ub_world_add_part(&w, ws, UB_CLASS_PART, NULL, "Ghost", &ghost);
    ub_ray_hit h;
    CHECK(ub_world_raycast(&w, ws, v3(0, 20, 0), down, 100, &h) && h.instance == shelf_i && fabsf(h.point.y - 5.5f) < 1e-4f);
    CHECK(ub_world_raycast(&w, ws, v3(10, 20, 10), down, 100, &h) && fabsf(h.point.y) < 1e-4f);
    CHECK(!ub_world_raycast(&w, ws, v3(200, 20, 0), down, 100, &h));
    ub_world_free(&w);
}

/* ------------------------------------------------- changing the tree */

static void test_mutation(void)
{
    ub_world w;
    build_sample(&w);
    i32 house = ub_world_find_child(&w, w.workspace, "House");
    i32 wall0 = ub_world_find_child(&w, house, "Wall0"), wall1 = ub_world_find_child(&w, house, "Wall1");
    i32 wall2 = ub_world_find_child(&w, house, "Wall2");
    CHECK(house >= 0 && wall0 >= 0 && wall1 >= 0 && wall2 >= 0);
    /* every part knows its instance */
    for (i32 i = 0; i < w.count; ++i)
        if (w.inst[i].part >= 0) CHECK(ub_world_part_inst(&w, w.inst[i].part) == i);
    /* reparent: Wall1 moves to the Workspace, the house keeps the others in order */
    CHECK(ub_world_set_parent(&w, wall1, w.workspace));
    CHECK(w.inst[wall1].parent == w.workspace && w.inst[w.workspace].last_child == wall1);
    CHECK(w.inst[house].first_child == wall0 && w.inst[wall0].next_sibling == wall2);
    CHECK(!ub_world_set_parent(&w, house, wall0));   /* into its own child: a loop */
    CHECK(!ub_world_set_parent(&w, w.workspace, house));
    /* clone the house: parts, names, the weld inside it */
    CHECK(ub_world_set_parent(&w, wall1, house));
    i32 parts_before = w.part_count, welds_before = w.weld_count;
    i32 copy = ub_world_clone(&w, house);
    CHECK(copy >= 0 && w.inst[copy].parent == -1 && (w.inst[copy].flags & UB_INST_DETACHED));
    CHECK(w.part_count == parts_before + 3 && w.weld_count == welds_before + 1);
    i32 cw0 = ub_world_find_child(&w, copy, "Wall0");
    CHECK(cw0 >= 0 && cw0 != wall0 && memcmp(&w.parts[w.inst[cw0].part], &w.parts[w.inst[wall0].part], sizeof(ub_part)) == 0);
    CHECK(strcmp(ub_world_str(&w, w.inst[ub_world_find_child(&w, ub_world_find_child(&w, copy, "Bits"), "Door script")].class_name), "Script") == 0);
    CHECK(ub_world_set_name(&w, copy, "House2") && ub_world_set_parent(&w, copy, w.workspace));
    CHECK(!(w.inst[copy].flags & UB_INST_DETACHED));
    /* destroy: the subtree is gone, its parts flagged, and it is not saved */
    i32 door = ub_world_find_child(&w, ub_world_find_child(&w, house, "Bits"), "Door script");
    CHECK(ub_world_set_source(&w, door, "print('hi')") && strcmp(ub_world_source(&w, door), "print('hi')") == 0);
    ub_world_destroy(&w, wall2);
    CHECK(!ub_world_alive(&w, wall2) && (w.parts[w.inst[wall2].part].flags & UB_PART_REMOVED));
    CHECK(!ub_world_set_parent(&w, wall2, w.workspace)); /* destroyed for good */
    i32 temp = ub_world_add(&w, -1, UB_CLASS_FOLDER, NULL, "Temp");
    w.inst[temp].flags |= UB_INST_DETACHED;
    i32 rt = ub_world_add(&w, w.workspace, UB_CLASS_MODEL, NULL, "Player1");
    w.inst[rt].flags |= UB_INST_RUNTIME;
    u8 *d1, *d2;
    u32 n1, n2;
    CHECK(ub_ubx_save(&w, &d1, &n1));
    ub_world b;
    ub_world_init(&b);
    CHECK(ub_ubx_load(&b, d1, n1) == UB_LOAD_OK);
    CHECK(ub_ubx_save(&b, &d2, &n2) && n1 == n2 && memcmp(d1, d2, n1) == 0);
    i32 bh = ub_world_find_child(&b, b.workspace, "House");
    CHECK(bh >= 0 && ub_world_find_child(&b, bh, "Wall2") < 0 && ub_world_find_child(&b, b.workspace, "House2") >= 0);
    CHECK(ub_world_find_child(&b, -1, "Temp") < 0 && ub_world_find_child(&b, b.workspace, "Player1") < 0);
    i32 bdoor = ub_world_find_child(&b, ub_world_find_child(&b, bh, "Bits"), "Door script");
    CHECK(bdoor >= 0 && strcmp(ub_world_source(&b, bdoor), "print('hi')") == 0);
    CHECK(b.weld_count == 2); /* the original and the copy's */
    for (i32 i = 0; i < b.count; ++i)
        if (b.inst[i].part >= 0) CHECK(ub_world_part_inst(&b, b.inst[i].part) == i);
    free(d1);
    free(d2);
    ub_world_free(&b);

    /* reclaiming: a destroyed instance's slots (and its part's) are reused,
     * its children lose their parent, welds to it go */
    i32 cwall0 = ub_world_find_child(&w, copy, "Wall0"), cwall1 = ub_world_find_child(&w, copy, "Wall1");
    i32 old_count = w.count, old_parts = w.part_count, welds = w.weld_count;
    i32 cpart = w.inst[cwall0].part;
    ub_world_reclaim(&w, cwall0); /* not destroyed: refused */
    CHECK(ub_world_alive(&w, cwall0) && w.free_inst_count == 0);
    ub_world_destroy(&w, copy);
    ub_world_reclaim(&w, cwall0);
    ub_world_reclaim(&w, cwall0); /* twice: no effect */
    CHECK(w.free_inst_count == 1 && w.free_part_count == 1 && w.weld_count == welds - 1);
    ub_world_reclaim(&w, copy);
    CHECK(w.inst[cwall1].parent == -1 && !ub_world_alive(&w, cwall1));
    CHECK(ub_world_live_count(&w) == old_count - 2);
    ub_part np;
    memset(&np, 0, sizeof np);
    np.rot = mat3_identity();
    np.size = v3(1, 1, 1);
    i32 fresh = ub_world_add_part(&w, w.workspace, UB_CLASS_PART, NULL, "Fresh", &np);
    CHECK(fresh == copy && w.inst[fresh].part == cpart && w.count == old_count && w.part_count == old_parts);
    CHECK(ub_world_alive(&w, fresh) && ub_world_part_inst(&w, cpart) == fresh && w.inst[fresh].first_child == -1);
    CHECK(ub_world_find_child(&w, w.workspace, "Fresh") == fresh && w.parts[cpart].flags == 0);
    i32 fresh2 = ub_world_add(&w, w.workspace, UB_CLASS_FOLDER, NULL, "Fresh2");
    CHECK(fresh2 == cwall0 && w.inst[fresh2].part == -1 && w.free_inst_count == 0);
    /* equal names share the string pool */
    u32 before = w.str_len;
    for (int i = 0; i < 100; ++i) CHECK(ub_world_set_name(&w, fresh2, "Same name"));
    CHECK(w.str_len <= before + 10);
    CHECK(ub_ubx_save(&w, &d1, &n1));
    ub_world_init(&b);
    CHECK(ub_ubx_load(&b, d1, n1) == UB_LOAD_OK && ub_world_find_child(&b, b.workspace, "Same name") >= 0);
    free(d1);
    ub_world_free(&b);
    ub_world_free(&w);

    /* a Roblox ModuleScript keeps its code */
    u32 n;
    u8 *data = load_file("tests/data/rbx/modulescript.rbxm", &n);
    CHECK(data != NULL);
    if (data) {
        ub_world_init(&w);
        ub_import_stats st;
        CHECK(ub_rbxl_load(&w, data, n, &st) == UB_LOAD_OK && st.scripts == 1);
        i32 ms = ub_world_find_child(&w, w.workspace, "ModuleScript");
        CHECK(ms >= 0 && strcmp(ub_world_source(&w, ms), "local module = {}\n\nreturn module\n") == 0);
        ub_world_free(&w);
        free(data);
    }
}

/* ---- rbx-test-files GUI models: every property the XML copy has must
 * be imported with the same value (tests/data/rbx/<name>.props.expected) */

static void inst_path(const ub_world *w, i32 i, char *buf, int cap)
{
    i32 chain[64];
    int n = 0;
    for (i32 k = i; k >= 0 && n < 64; k = w->inst[k].parent) chain[n++] = k;
    buf[0] = 0;
    int len = 0;
    for (int j = n - 2; j >= 0; --j) { /* skip the Workspace a model file is put in */
        const char *nm = ub_world_str(w, w->inst[chain[j]].name);
        len += snprintf(buf + len, (size_t)(cap - len), "%s%s", len ? "/" : "", nm);
        if (len >= cap) break;
    }
}

static int parse_nums(const char *s, double *out, int max)
{
    int n = 0;
    char *end;
    while (n < max) {
        double v = strtod(s, &end);
        if (end == s) break;
        out[n++] = v;
        s = end;
    }
    return n;
}

/* NaN is imported as 0 on purpose: it would poison layout */
static bool near(double a, double b)
{
    if (b != b) return a == 0;
    return fabs(a - b) <= 1e-5 * (fabs(b) > 1 ? fabs(b) : 1) || (a == b);
}

/* Does the instance's value match the expected line's type and text? */
static bool value_matches(const ub_world *w, const ub_value *v, const char *type, const char *text)
{
    double n[64];
    int k = 0;
    if (!strcmp(type, "string")) return v->type == UB_PV_STRING && strcmp(v->v.str.s, text) == 0;
    if (!strcmp(type, "ref")) {
        if (v->type != UB_PV_REF) return false;
        if (!text[0]) return v->v.ref == -1;
        char path[512];
        if (v->v.ref < 0) return false;
        inst_path(w, v->v.ref, path, sizeof path);
        return strcmp(path, text) == 0;
    }
    if (!strcmp(type, "font")) {
        k = parse_nums(text, n, 2);
        const char *fam = strchr(strchr(text, ' ') + 1, ' ') + 1;
        return v->type == UB_PV_FONT && k == 2 && v->v.font.weight == n[0] && v->v.font.style == n[1] &&
               strcmp(v->v.font.family, fam) == 0;
    }
    k = parse_nums(text, n, 64);
    if (!strcmp(type, "bool")) return v->type == UB_PV_BOOL && v->v.b == (n[0] != 0);
    if (!strcmp(type, "int")) /* the XML writes BrickColorValue.Value as a plain number */
        return (v->type == UB_PV_INT || v->type == UB_PV_BRICKCOLOR) && v->v.i == (i32)n[0];
    if (!strcmp(type, "enum")) return v->type == UB_PV_ENUM && v->v.e == (u32)n[0];
    if (!strcmp(type, "brickcolor")) return v->type == UB_PV_BRICKCOLOR && v->v.e == (u32)n[0];
    if (!strcmp(type, "double")) return v->type == UB_PV_DOUBLE && near(v->v.d, n[0]);
    if (!strcmp(type, "float")) return v->type == UB_PV_FLOAT && (near(v->v.f, n[0]) || (isinf(n[0]) && isinf(v->v.f)));
    if (!strcmp(type, "udim")) return v->type == UB_PV_UDIM && near(v->v.ud.s, n[0]) && v->v.ud.o == (i32)n[1];
    if (!strcmp(type, "udim2"))
        return v->type == UB_PV_UDIM2 && near(v->v.ud2.sx, n[0]) && v->v.ud2.ox == (i32)n[1] &&
               near(v->v.ud2.sy, n[2]) && v->v.ud2.oy == (i32)n[3];
    if (!strcmp(type, "numseq") || !strcmp(type, "colorseq")) {
        u8 t = type[0] == 'n' ? UB_PV_NUMSEQ : UB_PV_COLORSEQ;
        int stride = t == UB_PV_NUMSEQ ? 3 : 4;
        if (v->type != t || (int)v->v.seq.n * stride != k) return false;
        for (int j = 0; j < k; ++j)
            if (!near(v->v.seq.k[j], n[j])) return false;
        return true;
    }
    const struct { const char *name; u8 t; int count; } vec[] = {
        { "color3", UB_PV_COLOR3, 3 }, { "vector2", UB_PV_VECTOR2, 2 }, { "vector3", UB_PV_VECTOR3, 3 },
        { "rect", UB_PV_RECT, 4 }, { "numrange", UB_PV_NUMRANGE, 2 },
    };
    for (int j = 0; j < (int)(sizeof vec / sizeof vec[0]); ++j)
        if (!strcmp(type, vec[j].name)) {
            if (v->type != vec[j].t || k != vec[j].count) return false;
            const f32 *f = (const f32 *)&v->v;
            for (int q = 0; q < k; ++q)
                if (!near(f[q], n[q])) return false;
            return true;
        }
    return false;
}

static void check_rbx_props(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "tests/data/rbx/%s.rbxm", name);
    u32 size = 0;
    u8 *data = load_file(path, &size);
    CHECK(data != NULL);
    if (!data) return;
    ub_world w;
    ub_world_init(&w);
    ub_import_stats st;
    ub_load_result r = ub_rbxl_load(&w, data, size, &st);
    CHECK(r == UB_LOAD_OK);
    snprintf(path, sizeof path, "tests/data/rbx/%s.props.expected", name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    int lines = 0, bad = 0;
    char line[4096];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL, *c = b ? strchr(b + 1, '|') : NULL;
        if (!c) continue;
        *a = *b = *c = 0;
        lines++;
        /* any instance with this path that has the property with this value */
        bool found = false;
        for (i32 i = 0; i < w.count && !found; ++i) {
            char ip[512];
            inst_path(&w, i, ip, sizeof ip);
            if (strcmp(ip, line) != 0) continue;
            const ub_value *v = ub_world_prop_named(&w, i, a + 1);
            found = v && value_matches(&w, v, b + 1, c + 1);
        }
        if (!found) {
            if (bad < 5) printf("    %s: %s.%s is not %s %s\n", name, line, a + 1, b + 1, c + 1);
            bad++;
        }
    }
    if (f) fclose(f);
    printf("  %s: %d properties checked, %u GUI objects\n", name, lines, st.gui);
    CHECK(lines > 0 && bad == 0);
    /* and they survive .ubx */
    u8 *a1, *b1;
    u32 na, nb;
    CHECK(ub_ubx_save(&w, &a1, &na));
    ub_world w2;
    ub_world_init(&w2);
    CHECK(ub_ubx_load(&w2, a1, na) == UB_LOAD_OK);
    CHECK(ub_ubx_save(&w2, &b1, &nb) && na == nb && memcmp(a1, b1, na) == 0);
    plat_free(a1);
    plat_free(b1);
    ub_world_free(&w2);
    ub_world_free(&w);
    free(data);
}

static void test_rbx_gui(void)
{
    static const char *const models[] = {
        "three-unique-frames", "three-screengui", "three-uigradients", "three-uigridlayouts", "two-imagebuttons",
        "text-label-with-font", "gui-inset-and-font-migration", "imagelabel-content", "funny-uipadding",
        "three-intvalues", "three-brickcolorvalues", "funny-numbervalue", "three-color3values",
        "three-vector3values", "font",
    };
    for (int i = 0; i < (int)(sizeof models / sizeof models[0]); ++i) check_rbx_props(models[i]);
}

/* Typed properties: storing, copying with Clone, saving in .ubx, and the
 * class table's defaults and enums. */
static void test_props(void)
{
    ub_world w;
    ub_world_init(&w);
    i32 gui = ub_world_add(&w, -1, UB_CLASS_OTHER, "ScreenGui", "Hud");
    i32 label = ub_world_add(&w, gui, UB_CLASS_OTHER, "TextLabel", "Score");
    i32 link = ub_world_add(&w, gui, UB_CLASS_OTHER, "ObjectValue", "Target");
    CHECK(ub_cdb_fill_defaults(&w, label, "TextLabel"));
    CHECK(ub_cdb_fill_defaults(&w, link, "ObjectValue"));
    /* defaults of a new TextLabel, as Roblox has them */
    const ub_value *v = ub_world_prop_named(&w, label, "Text");
    CHECK(v && v->type == UB_PV_STRING && strcmp(v->v.str.s, "Label") == 0);
    v = ub_world_prop_named(&w, label, "TextSize");
    CHECK(v && v->type == UB_PV_FLOAT && v->v.f == 8);
    v = ub_world_prop_named(&w, label, "BorderSizePixel");
    CHECK(v && v->type == UB_PV_INT && v->v.i == 1);
    v = ub_world_prop_named(&w, label, "TextXAlignment");
    CHECK(v && v->type == UB_PV_ENUM && v->v.e == 2); /* Center */
    v = ub_world_prop_named(&w, label, "FontFace");
    CHECK(v && v->type == UB_PV_FONT && v->v.font.weight == 400 && strstr(v->v.font.family, "LegacyArial"));
    v = ub_world_prop_named(&w, link, "Value");
    CHECK(v && v->type == UB_PV_REF && v->v.ref == -1);
    CHECK(ub_cdb_is_a("TextButton", "GuiButton") && ub_cdb_is_a("TextButton", "GuiObject") &&
          ub_cdb_is_a("ScreenGui", "LayerCollector") && !ub_cdb_is_a("Frame", "GuiButton"));
    const ub_cdb_prop *pd = ub_cdb_find_prop(ub_cdb_class("TextButton"), "Size");
    CHECK(pd && pd->type == UB_PV_UDIM2 && (pd->flags & UB_CDB_STORED));
    pd = ub_cdb_find_prop(ub_cdb_class("Frame"), "AbsoluteSize");
    CHECK(pd && (pd->flags & UB_CDB_READONLY));
    int font = ub_enum_find("Font"), key = ub_enum_find("KeyCode");
    CHECK(font >= 0 && ub_enum_value_of(font, "Legacy") == 0 && ub_enum_value_of(font, "SourceSansBold") == 4);
    CHECK(key >= 0 && ub_enum_value_of(key, "W") == 119 && ub_enum_value_of(key, "LeftShift") == 304);

    /* setting: strings and sequences are copied */
    char text[16] = "Hello";
    ub_value s = ub_vstr(text, 5);
    CHECK(ub_world_set_prop_named(&w, label, "Text", &s));
    text[0] = 'J';
    CHECK(strcmp(ub_world_prop_named(&w, label, "Text")->v.str.s, "Hello") == 0);
    f32 keys[12] = { 0, 1, 0, 0, 0.5f, 0.25f, 0.5f, 0, 1, 0, 0, 1 };
    ub_value seq;
    seq.type = UB_PV_COLORSEQ;
    seq.v.seq.k = keys;
    seq.v.seq.n = 3;
    CHECK(ub_world_set_prop_named(&w, label, "Colours", &seq));
    keys[1] = 9;
    CHECK(ub_world_prop_named(&w, label, "Colours")->v.seq.k[1] == 1);
    seq.v.seq.n = 1; /* fewer than two keypoints: refused */
    CHECK(!ub_world_set_prop_named(&w, label, "Colours", &seq));
    ub_value ref;
    ref.type = UB_PV_REF;
    ref.v.ref = label;
    CHECK(ub_world_set_prop_named(&w, link, "Value", &ref));
    u32 e0 = w.epoch;
    ub_value ud;
    ud.type = UB_PV_UDIM2;
    ud.v.ud2.sx = 0.5f; ud.v.ud2.ox = -10; ud.v.ud2.sy = 0; ud.v.ud2.oy = 40;
    CHECK(ub_world_set_prop_named(&w, label, "Size", &ud) && w.epoch != e0);

    /* Clone copies properties, and references inside the copy point at the copies */
    i32 copy = ub_world_clone(&w, gui);
    CHECK(copy >= 0);
    i32 copy_label = ub_world_find_child(&w, copy, "Score"), copy_link = ub_world_find_child(&w, copy, "Target");
    CHECK(copy_label >= 0 && copy_link >= 0 && copy_label != label);
    v = ub_world_prop_named(&w, copy_label, "Text");
    CHECK(v && strcmp(v->v.str.s, "Hello") == 0 && v->v.str.s != ub_world_prop_named(&w, label, "Text")->v.str.s);
    v = ub_world_prop_named(&w, copy_link, "Value");
    CHECK(v && v->v.ref == copy_label);
    ub_world_destroy(&w, copy);
    ub_world_reclaim(&w, copy_link);
    CHECK(ub_world_prop_count(&w, copy_link) == 0);

    /* .ubx keeps every property; save -> load -> save gives the same bytes */
    u8 *data = NULL, *data2 = NULL;
    u32 size = 0, size2 = 0;
    CHECK(ub_ubx_save(&w, &data, &size));
    ub_world w2;
    ub_world_init(&w2);
    CHECK(ub_ubx_load(&w2, data, size) == UB_LOAD_OK);
    i32 l2 = ub_world_find_child(&w2, 0, "Score"), k2 = ub_world_find_child(&w2, 0, "Target");
    CHECK(l2 >= 0 && k2 >= 0);
    CHECK(ub_world_prop_count(&w2, l2) == ub_world_prop_count(&w, label));
    for (int i = 0; i < ub_world_prop_count(&w, label); ++i) {
        const ub_prop *p = ub_world_prop_at(&w, label, i);
        const ub_value *q = ub_world_prop_named(&w2, l2, ub_world_key_name(&w, p->key));
        CHECK(q && ub_value_equal(&p->val, q));
    }
    v = ub_world_prop_named(&w2, k2, "Value");
    CHECK(v && v->v.ref == l2);
    CHECK(ub_ubx_save(&w2, &data2, &size2));
    CHECK(size == size2 && memcmp(data, data2, size) == 0);
    /* damaged property data is refused, never misread */
    int refused = 0;
    for (u32 cut = 0; cut < size; cut += 7) {
        u8 *bad = (u8 *)malloc(size);
        memcpy(bad, data, size);
        bad[cut] ^= 0x5A;
        ub_world w3;
        ub_world_init(&w3);
        if (ub_ubx_load(&w3, bad, size) != UB_LOAD_OK) refused++;
        ub_world_free(&w3);
        free(bad);
    }
    CHECK(refused > 0);
    plat_free(data);
    plat_free(data2);
    ub_world_free(&w2);
    ub_world_free(&w);
}

void test_world(void)
{
    test_props();
    test_rbx_gui();
    test_mutation();
    test_raycast();
    test_special_rotations();
    test_ubx();
    test_rbxl();
}
