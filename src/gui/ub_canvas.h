/* Uniblox - a small 2D rasteriser for the GUI.
 *
 * Draws into 32-bit pixels with premultiplied alpha (0xAARRGGBB): filled
 * and rounded rectangles, rings (borders, UIStroke), gradients
 * (UIGradient), images (stretched, sliced, tiled) and coverage masks
 * (text). Shapes are given in their own box, [0, w] x [0, h], placed by an
 * affine transform (rotation and position), and anti-aliased at their
 * edges. Unrotated plain rectangles take a fast path.
 *
 * The same code draws the screen's GUI, BillboardGuis and SurfaceGuis, in
 * both renderers, so they look the same everywhere. */
#ifndef UB_CANVAS_H
#define UB_CANVAS_H

#include "core/ub_types.h"

typedef struct {
    u32 *px;                 /* premultiplied 0xAARRGGBB, rows top-down */
    int w, h, pitch;         /* pitch in pixels */
    int cx0, cy0, cx1, cy1;  /* clip rectangle (x1, y1 exclusive) */
} ub_canvas;

/* local (x, y) -> canvas: X = a x + c y + tx, Y = b x + d y + ty */
typedef struct {
    f32 a, b, c, d, tx, ty;
} ub_xform;

typedef struct {
    u32 color;               /* 0x00RRGGBB */
    f32 alpha;               /* 0..1 */
    /* UIGradient over the shape's box (multiplies colour and opacity) */
    const f32 *gcol;         /* keypoints (time, r, g, b), colours 0..1 */
    int gcol_n;
    const f32 *gtr;          /* keypoints (time, transparency, envelope) */
    int gtr_n;
    f32 gdx, gdy;            /* direction (unit), from the gradient's Rotation */
    f32 gox, goy;            /* its Offset */
} ub_paint;

typedef struct {
    const u32 *px;           /* premultiplied 0xAARRGGBB */
    int w, h;
} ub_image_px;

/* How one axis of an image maps onto one axis of the box: up to three
 * linear pieces (a 9-slice image has three), or tiles. */
typedef struct {
    int n;                   /* pieces */
    f32 d0[3], d1[3];        /* box coordinates */
    f32 s0[3], s1[3];        /* image coordinates (pixels) */
    f32 tile;                /* > 0: repeat every `tile` box units over [s0[0], s1[0]] */
} ub_axis_map;

void ub_canvas_init(ub_canvas *c, u32 *px, int w, int h, int pitch);
void ub_canvas_clip(ub_canvas *c, int x0, int y0, int x1, int y1);   /* intersected with the canvas */
void ub_canvas_clear(ub_canvas *c, int x0, int y0, int x1, int y1);  /* to transparent */

ub_xform ub_xform_place(f32 x, f32 y);                       /* box at (x, y), no rotation */
/* A box of size (w, h) whose top-left would be at (x, y), turned by
 * `degrees` (clockwise on screen) about its centre. */
ub_xform ub_xform_rotated(f32 x, f32 y, f32 w, f32 h, f32 degrees);
ub_xform ub_xform_mul(const ub_xform *outer, const ub_xform *inner);
bool ub_xform_is_plain(const ub_xform *t);                   /* only a translation */
/* Canvas point -> local point; false if the transform is singular. */
bool ub_xform_inverse(const ub_xform *t, f32 X, f32 Y, f32 *x, f32 *y);

/* A filled rectangle with corners of `radius`. */
void ub_canvas_rect(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, const ub_paint *p);
/* The band between the rectangle shrunk by `inner` and grown by `outer`
 * (a border: inner = thickness, outer = 0 for an inset border). */
void ub_canvas_ring(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, f32 inner, f32 outer,
                    const ub_paint *p);
/* An image over the box, through the two axis maps; tinted by the paint's
 * colour (multiplied); clipped to rounded corners of `radius`. */
void ub_canvas_image(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, const ub_image_px *img,
                     const ub_axis_map *mx, const ub_axis_map *my, bool nearest, const ub_paint *p);
/* A coverage mask (0..255, `mw` x `mh`) with its top-left at local (x, y),
 * in the paint's colour. The gradient, if any, spans the box (bw, bh). */
void ub_canvas_mask(ub_canvas *c, const ub_xform *t, f32 x, f32 y, const u8 *mask, int mw, int mh, f32 bw, f32 bh,
                    const ub_paint *p);

/* Gradient sampling (also used for text colours). t in 0..1. */
void ub_gradient_color(const f32 *keys, int n, f32 t, f32 *r, f32 *g, f32 *b);
f32 ub_gradient_number(const f32 *keys, int n, f32 t);

/* A plain stretch of the image's sub-rectangle onto [0, box]. */
void ub_axis_stretch(ub_axis_map *m, f32 box, f32 s0, f32 s1);
/* 9-slice: the outer parts keep their size (times scale). */
void ub_axis_slice(ub_axis_map *m, f32 box, f32 s0, f32 s1, f32 lo, f32 hi, f32 scale);
/* Tiles of `tile` box units. */
void ub_axis_tile(ub_axis_map *m, f32 box, f32 s0, f32 s1, f32 tile);

#endif
