#include "render/ub_palette.h"

u32 ub_palette[256];
static u8 g_gray_level[20];

void ub_palette_init(void)
{
    for (int i = 0; i < 256; ++i) ub_palette[i] = 0;
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b)
                ub_palette[UB_PAL_CUBE + r * 36 + g * 6 + b] =
                    ((u32)(r * 51) << 16) | ((u32)(g * 51) << 8) | (u32)(b * 51);
    /* 20 greys spread evenly; none coincide with the cube's multiples of 51. */
    for (int i = 0; i < 20; ++i) {
        u32 v = (u32)((255 * (2 * i + 1) + 20) / 40);
        g_gray_level[i] = (u8)v;
        ub_palette[UB_PAL_GRAY + i] = (v << 16) | (v << 8) | v;
    }
}

/* Bayer 2x2 thresholds, as offsets in 1/8ths of a cube step (51). */
static const int k_dither[4] = { -3, 1, 3, -1 };

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

u8 ub_palette_match(int r, int g, int b, int dither)
{
    r = clamp255(r);
    g = clamp255(g);
    b = clamp255(b);

    int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (hi - lo < 10) {
        /* Near-grey: pick the closest of the 26 greys (cube + extra). The
         * grey ramp is fine enough that dithering is not needed. */
        int y = (r * 77 + g * 151 + b * 28) >> 8;
        int best = UB_PAL_CUBE + ((y + 25) / 51) * 43; /* cube grey: r=g=b */
        int best_err = y - ((y + 25) / 51) * 51;
        if (best_err < 0) best_err = -best_err;
        for (int i = 0; i < 20; ++i) {
            int e = y - g_gray_level[i];
            if (e < 0) e = -e;
            if (e < best_err) {
                best_err = e;
                best = UB_PAL_GRAY + i;
            }
        }
        return (u8)best;
    }

    int off = dither >= 0 ? k_dither[dither & 3] * 51 / 8 : 0;
    int ri = clamp255(r + off) * 5 + 127;
    int gi = clamp255(g + off) * 5 + 127;
    int bi = clamp255(b + off) * 5 + 127;
    return (u8)(UB_PAL_CUBE + (ri / 255) * 36 + (gi / 255) * 6 + bi / 255);
}
