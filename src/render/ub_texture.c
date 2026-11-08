#include "render/ub_texture.h"

const ub_material_info ub_materials[UB_MAT_COUNT] = {
    { "plastic", 4.0f },
    { "grid", 4.0f },
    { "brick", 6.0f },
    { "wood", 4.0f },
    { "grass", 8.0f },
    { "metal", 4.0f },
    { "stone", 8.0f },
};

static u8 g_tex[UB_MAT_COUNT][UB_TEX_SIZE * UB_TEX_SIZE];

/* ------------------------------------------------------------ noise */

static u32 hash2(int x, int y, u32 seed)
{
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Tileable value noise: lattice of `cells` cells across the 64 texels,
 * smoothly interpolated. Returns 0..255. */
static int value_noise(int x, int y, int cells, u32 seed)
{
    int step = UB_TEX_SIZE / cells;
    int cx = x / step, cy = y / step;
    int fx = (x % step) * 256 / step, fy = (y % step) * 256 / step;
    /* smoothstep weights */
    fx = fx * fx * (768 - 2 * fx) >> 16;
    fy = fy * fy * (768 - 2 * fy) >> 16;
    int x1 = (cx + 1) % cells, y1 = (cy + 1) % cells;
    int a = (int)(hash2(cx, cy, seed) & 255), b = (int)(hash2(x1, cy, seed) & 255);
    int c = (int)(hash2(cx, y1, seed) & 255), d = (int)(hash2(x1, y1, seed) & 255);
    int top = a + ((b - a) * fx >> 8), bot = c + ((d - c) * fx >> 8);
    return top + ((bot - top) * fy >> 8);
}

static int fractal(int x, int y, u32 seed)
{
    return (value_noise(x, y, 4, seed) * 4 + value_noise(x, y, 8, seed + 1) * 2 +
            value_noise(x, y, 16, seed + 2) + value_noise(x, y, 32, seed + 3)) / 8;
}

static u8 level(int v)
{
    return (u8)(v < 0 ? 0 : v > 31 ? 31 : v);
}

/* ---------------------------------------------------------- textures */

static void gen_grid(u8 *t)
{
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int v = 22 + (fractal(x, y, 11) - 128) / 64;
            if (x < 2 || y < 2) v = 12;           /* grid line */
            else if (x < 3 || y < 3) v = 26;      /* bevel highlight */
            t[y * 64 + x] = level(v);
        }
}

static void gen_brick(u8 *t)
{
    /* 4 rows of bricks, 2 per row, every other row offset half a brick. */
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int row = y / 16, yy = y % 16;
            int xs = (x + (row & 1) * 16) % 64;
            int brick = xs / 32, xx = xs % 32;
            int v;
            if (yy < 2 || xx < 2) {
                v = 7 + (int)(hash2(x, y, 3) & 3);        /* mortar */
            } else {
                int shade = (int)(hash2(brick, row, 5) % 7) - 3; /* per-brick tone */
                v = 20 + shade + (fractal(x, y, 7) - 128) / 40;
                if (yy == 2 || xx == 2) v += 3;           /* top/left edge highlight */
            }
            t[y * 64 + x] = level(v);
        }
}

static void gen_wood(u8 *t)
{
    /* Grain runs along v (vertical stripes), wobbled by noise. */
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int warp = (value_noise(x, y, 4, 21) - 128) / 6;
            int p = ((x * 8 + warp) & 63);          /* 8 rings per tile */
            int tri = p < 32 ? p : 63 - p;          /* triangle wave 0..31 */
            int v = 13 + tri / 3 + (value_noise(x, y, 32, 23) - 128) / 48;
            t[y * 64 + x] = level(v);
        }
}

static void gen_grass(u8 *t)
{
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int v = 10 + fractal(x, y, 31) / 14 + (int)(hash2(x, y, 33) & 7) - 3;
            t[y * 64 + x] = level(v);
        }
}

static void gen_metal(u8 *t)
{
    /* Diamond tread plate over a brushed base. */
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int v = 16 + (value_noise(x, y / 16, 32, 41) - 128) / 40;
            int cx = x % 16, cy = y % 16;
            int up = (x / 16 + y / 16) & 1;      /* alternate diamond direction */
            int d = up ? (cx - cy) : (cx + cy - 15);
            if (d < 0) d = -d;
            if (d <= 1 && cx > 2 && cx < 13) v = 27;      /* raised ridge */
            else if (d == 2 && cx > 2 && cx < 13) v = 10; /* ridge shadow */
            t[y * 64 + x] = level(v);
        }
}

static void gen_stone(u8 *t)
{
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int n = fractal(x, y, 51);
            int crack = value_noise(x, y, 8, 53) - 128;
            if (crack < 0) crack = -crack;
            int v = 12 + n / 18;
            if (crack < 6) v -= 6;                 /* dark veins */
            t[y * 64 + x] = level(v);
        }
}

void ub_textures_init(void)
{
    for (int i = 0; i < UB_TEX_SIZE * UB_TEX_SIZE; ++i) g_tex[UB_MAT_PLASTIC][i] = 20;
    gen_grid(g_tex[UB_MAT_GRID]);
    gen_brick(g_tex[UB_MAT_BRICK]);
    gen_wood(g_tex[UB_MAT_WOOD]);
    gen_grass(g_tex[UB_MAT_GRASS]);
    gen_metal(g_tex[UB_MAT_METAL]);
    gen_stone(g_tex[UB_MAT_STONE]);
}

const u8 *ub_texture(ub_material m)
{
    if (m == UB_MAT_PLASTIC || (unsigned)m >= UB_MAT_COUNT) return NULL;
    return g_tex[m];
}
