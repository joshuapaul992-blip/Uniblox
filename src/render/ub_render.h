/* Uniblox - software renderer.
 *
 * Draws parts (boxes, wedges, cylinders, balls) with flat sun + sky
 * lighting, perspective-correct textures and a z-buffer, into either
 * framebuffer format (32-bit colour or 8-bit palette with dithering).
 *
 *   ub_render_begin(fb, &camera, &env);   clears z-buffer, draws the sky
 *   ub_render_part(&part);  ...           as many as needed, any order
 *   ub_render_end(&stats);
 *
 * Pipeline per part: bounding-sphere frustum cull -> transform vertices to
 * view space -> per face: back-face cull, lighting into a small colour
 * lookup table, clip against the near plane and the 4 screen edges,
 * project, scan-convert as one convex polygon. Spans interpolate 1/z,
 * u/z, v/z linearly and do a real divide every 16 pixels. */
#ifndef UB_RENDER_H
#define UB_RENDER_H

#include "core/ub_vec.h"
#include "platform/platform.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"

typedef struct {
    vec3 pos;
    f32 yaw, pitch;      /* radians; yaw 0 looks along +Z, pitch > 0 looks up */
    f32 fov_y;           /* vertical field of view, radians */
    f32 far_dist;        /* parts beyond this are culled */
} ub_camera;

typedef struct {
    vec3 sun_dir;        /* unit vector pointing towards the sun */
    f32 sun_strength;    /* 0..1 */
    f32 ambient;         /* 0..1 sky light on upward faces */
    u32 sky_top, sky_horizon, ground_color; /* 0x00RRGGBB */
} ub_render_env;

typedef struct {
    vec3 pos;
    mat3 rot;
    vec3 size;
    u32 color;           /* 0x00RRGGBB */
    u8 shape;            /* ub_shape */
    u8 material;         /* ub_material */
} ub_part_draw;

typedef struct {
    u32 parts, parts_culled;
    u32 faces, faces_backfacing, polys_drawn;
    u32 pixels_tested;
    u32 luts_built;      /* colour tables (re)built this frame (cache misses) */
    u32 render_us;       /* begin to end */
} ub_render_stats;

bool ub_render_init(void);
void ub_render_shutdown(void);

void ub_render_begin(ub_framebuffer *fb, const ub_camera *cam, const ub_render_env *env);
void ub_render_part(const ub_part_draw *p);
void ub_render_end(ub_render_stats *stats);

/* The 8-bit palette, for ub_framebuffer.palette. */
const u32 *ub_render_palette(void);

static inline ub_render_env ub_render_default_env(void)
{
    ub_render_env e;
    e.sun_dir = v3_normalize(v3(0.45f, 0.8f, -0.35f));
    e.sun_strength = 0.62f;
    e.ambient = 0.38f;
    e.sky_top = 0x3A6FD8;
    e.sky_horizon = 0xBFD8F2;
    e.ground_color = 0x55703F;
    return e;
}

#endif
