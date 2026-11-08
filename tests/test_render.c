/* Uniblox - host tests for vector maths, the palette and the renderer. */
#include <math.h>
#include <stdlib.h>

#include "core/ub_vec.h"
#include "render/ub_palette.h"
#include "render/ub_render.h"
#include "test.h"

#define W 320
#define H 240

static u32 g_px32[W * H], g_sky32[W * H];
static u8 g_px8[W * H], g_sky8[W * H];

static ub_framebuffer make_fb(ub_pixel_format fmt)
{
    ub_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    fb.format = fmt;
    fb.pixels = g_px32;
    fb.pixels8 = g_px8;
    fb.width = W;
    fb.height = H;
    fb.pitch = W;
    return fb;
}

static bool near1(vec3 a, vec3 b)
{
    return fabsf(a.x - b.x) < 1e-4f && fabsf(a.y - b.y) < 1e-4f && fabsf(a.z - b.z) < 1e-4f;
}

/* ---------------------------------------------------------------- vec */

void test_vec(void)
{
    mat3 m = mat3_from_euler(0, 0, 0);
    CHECK(near1(m.x, v3(1, 0, 0)) && near1(m.y, v3(0, 1, 0)) && near1(m.z, v3(0, 0, 1)));

    m = mat3_from_euler(UB_HALF_PI, 0, 0); /* yaw 90: forward becomes +X */
    CHECK(near1(m.z, v3(1, 0, 0)) && near1(m.x, v3(0, 0, -1)));
    m = mat3_from_euler(0, UB_HALF_PI, 0); /* pitch 90: forward becomes +Y */
    CHECK(near1(m.z, v3(0, 1, 0)));
    m = mat3_from_euler(0, 0, UB_HALF_PI); /* roll 90: X becomes +Y */
    CHECK(near1(m.x, v3(0, 1, 0)));

    /* any rotation is orthonormal and right-handed in the algebraic sense */
    for (int i = 0; i < 200; ++i) {
        m = mat3_from_euler(i * 0.37f, i * 0.11f - 1.2f, i * 0.23f);
        CHECK(fabsf(v3_length(m.x) - 1) < 1e-4f && fabsf(v3_length(m.z) - 1) < 1e-4f);
        CHECK(fabsf(v3_dot(m.x, m.y)) < 1e-4f && fabsf(v3_dot(m.y, m.z)) < 1e-4f);
        CHECK(near1(v3_cross(m.x, m.y), m.z));
    }
    CHECK(fabsf(ub_sqrtf(2.0f) - 1.41421356f) < 1e-6f);
    CHECK(ub_floorf(-1.5f) == -2 && ub_floorf(1.5f) == 1 && ub_floorf(-2.0f) == -2);
    CHECK(ub_ceilf(-1.5f) == -1 && ub_ceilf(1.5f) == 2 && ub_ceilf(2.0f) == 2);
}

/* ------------------------------------------------------------ palette */

void test_palette(void)
{
    ub_palette_init();
    CHECK(ub_palette[UB_PAL_CUBE] == 0x000000);
    CHECK(ub_palette[UB_PAL_CUBE + 215] == 0xFFFFFF);
    CHECK(ub_palette_match(255, 0, 0, -1) == UB_PAL_CUBE + 5 * 36);
    CHECK(ub_palette_match(0, 0, 255, -1) == UB_PAL_CUBE + 5);
    CHECK(ub_palette_match(102, 204, 51, -1) == UB_PAL_CUBE + 2 * 36 + 4 * 6 + 1);
    /* greys use the finer grey ramp: error at most half a step (~6.4) */
    for (int v = 0; v < 256; ++v) {
        u32 c = ub_palette[ub_palette_match(v, v, v, -1)];
        int got = (int)(c & 255);
        CHECK(abs(got - v) <= 7 && ((c >> 16) & 255) == (c & 255));
    }
    /* never uses Windows' reserved entries, for any input or dither cell */
    bool in_range = true;
    for (int i = 0; i < 20000; ++i) {
        u8 k = ub_palette_match(rand() % 300 - 20, rand() % 300 - 20, rand() % 300 - 20, rand() % 5 - 1);
        if (k < UB_PAL_FIRST || k > UB_PAL_LAST) in_range = false;
    }
    CHECK(in_range);
    /* dithering a mid colour mixes neighbouring cube levels */
    u8 d0 = ub_palette_match(128, 20, 20, 0), d2 = ub_palette_match(128, 20, 20, 2);
    CHECK(d0 != d2);
}

/* ------------------------------------------------------------- render */

static ub_camera cam_at(vec3 pos, f32 yaw, f32 pitch, f32 fov_deg)
{
    ub_camera c;
    c.pos = pos;
    c.yaw = yaw;
    c.pitch = pitch;
    c.fov_y = fov_deg * UB_PI / 180.0f;
    c.far_dist = 1000.0f;
    return c;
}

static ub_part_draw part(ub_shape shape, vec3 pos, vec3 size, u32 color, ub_material mat)
{
    ub_part_draw p;
    p.pos = pos;
    p.rot = mat3_identity();
    p.size = size;
    p.color = color;
    p.shape = (u8)shape;
    p.material = (u8)mat;
    return p;
}

/* Flat, even lighting so colours are predictable. */
static ub_render_env flat_env(void)
{
    ub_render_env e = ub_render_default_env();
    e.sun_strength = 0.0f;
    e.ambient = 1.0f;
    return e;
}

/* Render the sky alone into g_sky*, for "is this pixel covered" checks. */
static void render_sky(ub_framebuffer *fb, const ub_camera *c, const ub_render_env *e)
{
    ub_render_begin(fb, c, e);
    ub_render_end(NULL);
    memcpy(g_sky32, g_px32, sizeof g_sky32);
    memcpy(g_sky8, g_px8, sizeof g_sky8);
}

static bool covered(const ub_framebuffer *fb, int x, int y)
{
    return fb->format == UB_PIXEL_XRGB32 ? g_px32[y * W + x] != g_sky32[y * W + x]
                                         : g_px8[y * W + x] != g_sky8[y * W + x];
}

static int count_uncovered(const ub_framebuffer *fb)
{
    int n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) n += !covered(fb, x, y);
    return n;
}

void test_render(void)
{
    CHECK(ub_render_init());
    ub_render_env env = flat_env();

    for (int pass = 0; pass < 2; ++pass) {
        ub_framebuffer fb = make_fb(pass ? UB_PIXEL_INDEX8 : UB_PIXEL_XRGB32);
        printf("  %s\n", pass ? "8-bit" : "32-bit");

        /* Exact coverage: a 4-unit cube whose front face is 8 units away.
         * focal = 120 / tan(35 deg) = 171.38, half-width 2/8*focal = 42.85,
         * so pixel centres 117.5 .. 202.5 are inside: 86 pixels. */
        ub_camera cam = cam_at(v3(0, 0, -10), 0, 0, 70);
        render_sky(&fb, &cam, &env);
        ub_render_begin(&fb, &cam, &env);
        ub_part_draw red = part(UB_SHAPE_BOX, v3(0, 0, 0), v3(4, 4, 4), 0xFF0000, UB_MAT_PLASTIC);
        ub_render_part(&red);
        ub_render_stats st;
        ub_render_end(&st);
        int row = 0, first = -1, last = -1;
        for (int x = 0; x < W; ++x)
            if (covered(&fb, x, H / 2)) { row++; if (first < 0) first = x; last = x; }
        CHECK(row == 86 && first == 117 && last == 202);
        CHECK(st.parts == 1 && st.polys_drawn == 1 && st.faces_backfacing == 5);
        /* flat light on a side face is 0.8 (quantised to 204/256): 255 -> 203 */
        if (!pass) CHECK(g_px32[(H / 2) * W + W / 2] == 0xCB0000);
        else CHECK(ub_palette[g_px8[(H / 2) * W + W / 2]] == 0xCC0000);

        /* Occlusion, in both draw orders: the nearer green box wins. */
        ub_part_draw green = part(UB_SHAPE_BOX, v3(0, 0, -4), v3(2, 2, 2), 0x00FF00, UB_MAT_PLASTIC);
        for (int order = 0; order < 2; ++order) {
            ub_render_begin(&fb, &cam, &env);
            ub_render_part(order ? &green : &red);
            ub_render_part(order ? &red : &green);
            ub_render_end(NULL);
            if (!pass) {
                u32 c = g_px32[(H / 2) * W + W / 2];
                CHECK((c >> 8 & 255) > 200 && (c >> 16 & 255) < 50);
                u32 e = g_px32[(H / 2) * W + 110 + 10]; /* red box edge, outside green */
                CHECK((e >> 16 & 255) > 200);
            } else {
                CHECK(ub_palette[g_px8[(H / 2) * W + W / 2]] == 0x00CC00);
            }
        }

        /* Two boxes sharing an edge: no gap and no sky between them. */
        ub_part_draw l = part(UB_SHAPE_BOX, v3(-2, 0, 0), v3(4, 4, 4), 0x2050C0, UB_MAT_PLASTIC);
        ub_part_draw r = part(UB_SHAPE_BOX, v3(2, 0, 0), v3(4, 4, 4), 0xC05020, UB_MAT_PLASTIC);
        ub_render_begin(&fb, &cam, &env);
        ub_render_part(&l);
        ub_render_part(&r);
        ub_render_end(NULL);
        int gaps = 0;
        for (int y = 90; y < 150; ++y)
            for (int x = 100; x < 220; ++x) gaps += !covered(&fb, x, y);
        CHECK(gaps == 0);

        /* Watertight: up close to a cube's corner every pixel is covered,
         * so the three faces meet without seams and big faces clip cleanly.
         * The cube's far edges are ~33 degrees off the view axis; a 36 degree
         * field of view reaches ~28 degrees into the screen corners. */
        vec3 eye = v3(5.6f, 5.6f, -5.6f);
        f32 yaw = atan2f(-eye.x, -eye.z), pitch = -asinf(1.0f / sqrtf(3.0f));
        cam = cam_at(eye, yaw, pitch, 36);
        render_sky(&fb, &cam, &env);
        for (int m = 0; m < 2; ++m) {
            ub_part_draw big = part(UB_SHAPE_BOX, v3(0, 0, 0), v3(10, 10, 10), 0xFF00FF,
                                    m ? UB_MAT_BRICK : UB_MAT_PLASTIC);
            ub_render_begin(&fb, &cam, &env);
            ub_render_part(&big);
            ub_render_end(&st);
            CHECK(count_uncovered(&fb) == 0);
            CHECK(st.polys_drawn == 3);
        }

        /* Camera inside a box: everything back-facing, nothing drawn. */
        cam = cam_at(v3(0, 0, 0), 0.3f, 0.2f, 70);
        render_sky(&fb, &cam, &env);
        ub_render_begin(&fb, &cam, &env);
        ub_render_part(&red);
        ub_render_end(&st);
        CHECK(count_uncovered(&fb) == W * H && st.faces_backfacing == 6);

        /* Behind the camera / beyond far: culled before any face work. */
        cam = cam_at(v3(0, 0, -10), UB_PI, 0, 70);
        ub_render_begin(&fb, &cam, &env);
        ub_render_part(&red);
        ub_part_draw far = part(UB_SHAPE_BOX, v3(0, 0, 5000), v3(4, 4, 4), 0xFF0000, UB_MAT_PLASTIC);
        ub_render_part(&far);
        ub_render_end(&st);
        CHECK(st.parts_culled == 2 && st.faces == 0);

        /* Textured floor: covered, and the texture actually varies. */
        cam = cam_at(v3(0, 3, -20), 0, -0.3f, 70);
        render_sky(&fb, &cam, &env);
        ub_part_draw floor = part(UB_SHAPE_BOX, v3(0, -0.5f, 0), v3(400, 1, 400), 0xC06040, UB_MAT_GRID); /* unlike the green ground colour, even dithered */
        ub_render_begin(&fb, &cam, &env);
        ub_render_part(&floor);
        ub_render_end(&st);
        /* lower half is all floor; the grid texture gives at least panel,
         * line and highlight colours */
        int floor_gaps = 0, line_px = 0;
        u32 base = pass ? g_px8[(H - 1) * W + W / 2 + 7] : g_px32[(H - 1) * W + W / 2 + 7];
        for (int y = H / 2; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                floor_gaps += !covered(&fb, x, y);
                u32 c = pass ? g_px8[y * W + x] : g_px32[y * W + x];
                line_px += c != base;
            }
        CHECK(floor_gaps == 0);
        CHECK(line_px > 500);

        /* Every shape renders something when in view. */
        cam = cam_at(v3(0, 0, -10), 0, 0, 70);
        for (int s = 0; s < UB_SHAPE_COUNT; ++s) {
            ub_part_draw sp = part((ub_shape)s, v3(0, 0, 0), v3(4, 4, 4), 0xFFFF00, UB_MAT_WOOD);
            sp.rot = mat3_from_euler(0.5f, 0.4f, 0.0f);
            ub_render_begin(&fb, &cam, &env);
            ub_render_part(&sp);
            ub_render_end(&st);
            CHECK(st.polys_drawn >= 1 && st.pixels_tested > 300);
        }
    }
    ub_render_shutdown();
}

/* Random scenes and cameras (including inside parts and grazing angles).
 * The point is AddressSanitizer/UBSan: no out-of-bounds writes, no
 * overflow, no NaN-to-int conversions. */
void test_render_fuzz(void)
{
    CHECK(ub_render_init());
    ub_render_env env = ub_render_default_env();
    srand(1234);
    u32 polys = 0;
    for (int frame = 0; frame < 400; ++frame) {
        ub_framebuffer fb = make_fb(frame & 1 ? UB_PIXEL_INDEX8 : UB_PIXEL_XRGB32);
        ub_camera cam = cam_at(v3((rand() % 2000 - 1000) / 40.0f, (rand() % 2000 - 1000) / 40.0f,
                                  (rand() % 2000 - 1000) / 40.0f),
                               (rand() % 6283) / 1000.0f, (rand() % 3000 - 1500) / 1000.0f,
                               (f32)(20 + rand() % 120));
        ub_render_begin(&fb, &cam, &env);
        for (int i = 0; i < 40; ++i) {
            ub_part_draw p = part((ub_shape)(rand() % UB_SHAPE_COUNT),
                                  v3((rand() % 2000 - 1000) / 40.0f, (rand() % 2000 - 1000) / 40.0f,
                                     (rand() % 2000 - 1000) / 40.0f),
                                  v3((rand() % 4000) / 100.0f + 0.001f, (rand() % 4000) / 100.0f + 0.001f,
                                     (rand() % 4000) / 100.0f + 0.001f),
                                  (u32)rand() & 0xFFFFFF, (ub_material)(rand() % UB_MAT_COUNT));
            p.rot = mat3_from_euler((rand() % 6283) / 1000.0f, (rand() % 6283) / 1000.0f, (rand() % 6283) / 1000.0f);
            ub_render_part(&p);
        }
        ub_render_stats st;
        ub_render_end(&st);
        polys += st.polys_drawn;
    }
    printf("  400 random frames, %u polygons drawn\n", polys);
    CHECK(polys > 1000);
    ub_render_shutdown();
}
