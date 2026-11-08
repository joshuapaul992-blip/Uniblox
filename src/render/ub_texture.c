#include "render/ub_texture.h"
#include "platform/platform.h"

const ub_material_info ub_materials[UB_MAT_COUNT] = {
    /* name      repeat  specular shininess */
    { "plastic", 4.0f,   0.40f,   48.0f },
    { "grid",    4.0f,   0.18f,   24.0f },
    { "brick",   6.0f,   0.04f,   8.0f },
    { "wood",    4.0f,   0.12f,   16.0f },
    { "grass",   8.0f,   0.00f,   4.0f },
    { "metal",   4.0f,   0.70f,   64.0f },
    { "stone",   8.0f,   0.06f,   10.0f },
};

static u8 g_tex[UB_MAT_COUNT][UB_TEX_SIZE * UB_TEX_SIZE];

/* ------------------------------------------------------------ noise */

static u32 hash2(int x, int y, u32 seed)
{
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Tileable value noise over the 64-unit texture tile, `cells` lattice
 * cells across, evaluated at continuous coordinates. Returns 0..255. */
static f32 value_noise(f32 x, f32 y, int cells, u32 seed)
{
    f32 step = 64.0f / (f32)cells;
    f32 gx = x / step, gy = y / step;
    int cx = ub_floorf(gx), cy = ub_floorf(gy);
    f32 fx = gx - (f32)cx, fy = gy - (f32)cy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    int x0 = ((cx % cells) + cells) % cells, y0 = ((cy % cells) + cells) % cells;
    int x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
    f32 a = (f32)(hash2(x0, y0, seed) & 255), b = (f32)(hash2(x1, y0, seed) & 255);
    f32 c = (f32)(hash2(x0, y1, seed) & 255), d = (f32)(hash2(x1, y1, seed) & 255);
    f32 top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

static f32 fractal(f32 x, f32 y, u32 seed)
{
    return (value_noise(x, y, 4, seed) * 4 + value_noise(x, y, 8, seed + 1) * 2 +
            value_noise(x, y, 16, seed + 2) + value_noise(x, y, 32, seed + 3)) / 8;
}

/* -------------------------------------------------------- generators */

/* Each returns brightness 0..31 at tile coordinates (x, y) in 0..64.
 * `px` is the size of one output texel in tile units (1 at 64x64), used
 * for fine per-texel grain and to keep thin lines at least a texel wide. */
typedef f32 (*gen_fn)(f32 x, f32 y, f32 px, int ix, int iy);

static f32 fwrap(f32 v, f32 m)
{
    v -= (f32)ub_floorf(v / m) * m;
    return v;
}

static f32 gen_grid(f32 x, f32 y, f32 px, int ix, int iy)
{
    (void)ix; (void)iy;
    f32 v = 22.0f + (fractal(x, y, 11) - 128.0f) / 64.0f;
    f32 lx = fwrap(x, 64.0f), ly = fwrap(y, 64.0f);
    f32 line = 2.0f > px ? 2.0f : px;
    if (lx < line || ly < line) v = 12.0f;                       /* grid line */
    else if (lx < line + 1.0f || ly < line + 1.0f) v = 26.0f;    /* bevel highlight */
    return v;
}

static f32 gen_brick(f32 x, f32 y, f32 px, int ix, int iy)
{
    /* 4 rows of bricks, 2 per row, every other row offset half a brick. */
    int row = ub_floorf(y / 16.0f) & 3;
    f32 yy = fwrap(y, 16.0f);
    f32 xs = fwrap(x + (f32)(row & 1) * 16.0f, 64.0f);
    int brick = (int)(xs / 32.0f);
    f32 xx = fwrap(xs, 32.0f);
    f32 mortar = 2.0f > px ? 2.0f : px;
    if (yy < mortar || xx < mortar)
        return 7.0f + (f32)(hash2(ix, iy, 3) & 3);
    f32 shade = (f32)(hash2(brick, row, 5) % 7) - 3.0f;          /* per-brick tone */
    f32 v = 20.0f + shade + (fractal(x, y, 7) - 128.0f) / 40.0f;
    if (yy < mortar + px || xx < mortar + px) v += 3.0f;         /* edge highlight */
    return v;
}

static f32 gen_wood(f32 x, f32 y, f32 px, int ix, int iy)
{
    (void)px; (void)ix; (void)iy;
    /* grain runs along v, wobbled by noise; 8 rings per tile */
    f32 warp = (value_noise(x, y, 4, 21) - 128.0f) / 6.0f;
    f32 p = fwrap(x * 8.0f + warp, 64.0f);
    f32 tri = p < 32.0f ? p : 64.0f - p;
    return 13.0f + tri / 3.0f + (value_noise(x, y, 32, 23) - 128.0f) / 48.0f;
}

static f32 gen_grass(f32 x, f32 y, f32 px, int ix, int iy)
{
    (void)px;
    return 10.0f + fractal(x, y, 31) / 14.0f + (f32)(hash2(ix, iy, 33) & 7) - 3.0f;
}

static f32 gen_metal(f32 x, f32 y, f32 px, int ix, int iy)
{
    (void)ix; (void)iy;
    /* diamond tread plate over a brushed base */
    f32 v = 16.0f + (value_noise(x, y / 16.0f, 32, 41) - 128.0f) / 40.0f;
    f32 cx = fwrap(x, 16.0f), cy = fwrap(y, 16.0f);
    int up = (ub_floorf(x / 16.0f) + ub_floorf(y / 16.0f)) & 1;
    f32 d = up ? (cx - cy) : (cx + cy - 15.0f);
    if (d < 0) d = -d;
    f32 w = px > 1.0f ? px : 1.0f;
    if (cx > 2.0f && cx < 13.0f) {
        if (d <= w) v = 27.0f;                 /* raised ridge */
        else if (d <= 2.0f * w) v = 10.0f;     /* ridge shadow */
    }
    return v;
}

static f32 gen_stone(f32 x, f32 y, f32 px, int ix, int iy)
{
    (void)px; (void)ix; (void)iy;
    f32 n = fractal(x, y, 51);
    f32 crack = value_noise(x, y, 8, 53) - 128.0f;
    if (crack < 0) crack = -crack;
    f32 v = 12.0f + n / 18.0f;
    if (crack < 6.0f) v -= 6.0f * (1.0f - crack / 6.0f) + 2.0f;   /* dark veins */
    return v;
}

static const gen_fn k_gen[UB_MAT_COUNT] = {
    NULL, gen_grid, gen_brick, gen_wood, gen_grass, gen_metal, gen_stone
};

static f32 clamp31(f32 v)
{
    return v < 0 ? 0 : v > 31.0f ? 31.0f : v;
}

void ub_textures_init(void)
{
    for (int i = 0; i < UB_TEX_SIZE * UB_TEX_SIZE; ++i) g_tex[UB_MAT_PLASTIC][i] = 20;
    for (int m = 1; m < UB_MAT_COUNT; ++m)
        for (int y = 0; y < UB_TEX_SIZE; ++y)
            for (int x = 0; x < UB_TEX_SIZE; ++x)
                g_tex[m][y * UB_TEX_SIZE + x] =
                    (u8)clamp31(k_gen[m]((f32)x + 0.5f, (f32)y + 0.5f, 1.0f, x, y) + 0.5f);
}

const u8 *ub_texture(ub_material m)
{
    if (m == UB_MAT_PLASTIC || (unsigned)m >= UB_MAT_COUNT) return NULL;
    return g_tex[m];
}

u8 *ub_texture_hd(ub_material m, int size)
{
    if (m == UB_MAT_PLASTIC || (unsigned)m >= UB_MAT_COUNT || size < 64) return NULL;
    u8 *t = (u8 *)plat_alloc((size_t)size * size);
    if (!t) return NULL;
    f32 px = 64.0f / (f32)size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            f32 v = k_gen[m](((f32)x + 0.5f) * px, ((f32)y + 0.5f) * px, px, x, y);
            t[y * size + x] = (u8)(clamp31(v) * (255.0f / 31.0f) + 0.5f);
        }
    return t;
}
