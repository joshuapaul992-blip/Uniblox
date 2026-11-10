/* Uniblox - renderer interface.
 *
 * Two backends draw the same scene description:
 *   - software: z-buffered, perspective-textured polygons into a
 *     ub_framebuffer (32-bit or 8-bit palette); runs on anything
 *   - OpenGL: hardware rendering at native resolution with per-pixel
 *     lighting, sun shadows, specular highlights, fog, a sky with a sun,
 *     anti-aliasing and filtered textures (shader path, GL 2.0+), or
 *     per-vertex lighting and fog on plain GL 1.1
 *
 * Per frame:
 *   ub_render_begin(frame, &camera, &env);
 *   ub_render_part(&part); ...          parts are queued
 *   ub_render_end_scene();              the 3D scene is drawn
 *   ub_render_text(x, y, "text", color); ...   2D text on top
 *   ub_render_end(&stats);
 */
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
    vec3 focus;          /* point of interest (shadows are sharpest around it) */
} ub_camera;

typedef struct {
    vec3 sun_dir;        /* unit vector pointing towards the sun */
    f32 sun_strength;    /* 0..1, software backend */
    f32 ambient;         /* 0..1, software backend */
    u32 sky_top, sky_horizon, ground_color; /* 0x00RRGGBB */
    u32 sun_color;       /* OpenGL: direct sunlight colour */
    u32 sky_light, ground_light; /* OpenGL: ambient light from above / below */
    f32 fog_density;     /* OpenGL: per world unit (0 = none) */
} ub_render_env;

typedef struct {
    vec3 pos;
    mat3 rot;
    vec3 size;
    u32 color;           /* 0x00RRGGBB */
    u8 shape;            /* ub_shape */
    u8 material;         /* ub_material */
    u8 flags;            /* UB_DRAW_* */
} ub_part_draw;

enum { UB_DRAW_GLOW = 1 }; /* lit at full brightness (neon) */

/* See-through parts: flags bits 4..7 say how many sixteenths of the part's
 * pixels are left out, in a fixed dither pattern ("screen door"). Needs no
 * sorting and looks the same in both renderers and in 8-bit colour. */
#define UB_DRAW_SEE_SHIFT 4
static inline u8 ub_draw_see_through(u8 transparency) /* 0..255 -> flags bits */
{
    u32 level = ((u32)transparency * 16u + 127u) / 255u;
    return (u8)((level > 15 ? 15 : level) << UB_DRAW_SEE_SHIFT);
}
static inline int ub_draw_see_level(u8 flags) { return (flags >> UB_DRAW_SEE_SHIFT) & 15; }
/* the 4x4 ordered-dither pattern: a pixel is left out when k < level */
extern const u8 ub_see_through_pattern[4][4];

typedef enum {
    UB_QUALITY_LOW,      /* OpenGL: no shadows, no anti-aliasing */
    UB_QUALITY_MEDIUM,   /* OpenGL: shadows */
    UB_QUALITY_HIGH,     /* OpenGL: shadows + 4x anti-aliasing */
    UB_QUALITY_COUNT
} ub_quality;

typedef struct {
    u32 parts, parts_culled;
    u32 faces, faces_backfacing, polys_drawn;
    u32 pixels_tested;   /* software backend */
    u32 luts_built;      /* software backend: colour tables (re)built */
    u32 triangles;       /* OpenGL backend: triangles submitted (main pass) */
    u32 render_us;       /* CPU time from begin to end */
} ub_render_stats;

/* Call once the platform has chosen the backend (and, for OpenGL, made a
 * context current). Fills `desc` with a one-line description. */
bool ub_render_init(const ub_frame *frame, char *desc, int desc_len);
void ub_render_shutdown(void);
ub_backend ub_render_backend(void);

/* OpenGL: call before ub_render_init to use the plain GL 1.1 path even
 * when shaders are available (for drivers with broken shader support). */
void ub_render_force_fixed_function(bool on);

/* OpenGL only; the software backend ignores it. */
void ub_render_set_quality(ub_quality q);
ub_quality ub_render_quality(void);
const char *ub_render_quality_name(ub_quality q);

/* Parts that do not move, drawn every frame without being passed again.
 * The OpenGL renderer builds their geometry once (in GPU memory when the
 * driver has vertex buffers). Call again whenever they change; 0 clears. */
void ub_render_set_static(const ub_part_draw *parts, int count);

void ub_render_begin(ub_frame *frame, const ub_camera *cam, const ub_render_env *env);
void ub_render_part(const ub_part_draw *p);
void ub_render_end_scene(void);
/* Text in window pixels; (x, y) is the top-left of the line. Drawn with a
 * drop shadow unless the colour has UB_TEXT_FLAT set. */
#define UB_TEXT_FLAT 0x01000000u
void ub_render_text(int x, int y, const char *utf8, u32 color);
int ub_render_line_height(void);

/* ---- 2D for tools and menus (after the scene; window pixels) ---- */
/* Font size in pixels; 0 = follows the window height (the default). */
void ub_render_set_text_px(int px);
/* Software renderer: draw text into the frame itself, in order with
 * rectangles (for overlapping panels and menus), instead of having the OS
 * draw it on top afterwards. Sharpest when the frame is window-sized. */
void ub_render_set_text_in_frame(bool on);
int ub_render_text_width(const char *utf8);
/* Width of the first n bytes. */
int ub_render_text_width_n(const char *utf8, int n);
void ub_render_rect(int x, int y, int w, int h, u32 color);
/* Clip 2D drawing (rects and in-frame text) to a rectangle; w <= 0 turns
 * it off. */
void ub_render_clip(int x, int y, int w, int h);

/* The ray through window pixel (x, y), as the renderers project; dir is
 * unit length. */
void ub_render_screen_ray(const ub_camera *cam, int width, int height, f32 x, f32 y, vec3 *origin, vec3 *dir);
/* Where a point lands in the window; false if it is behind the camera. */
bool ub_render_project(const ub_camera *cam, int width, int height, vec3 p, f32 *sx, f32 *sy);
void ub_render_end(ub_render_stats *stats);

/* The 8-bit palette, for ub_framebuffer.palette. */
const u32 *ub_render_palette(void);

static inline ub_render_env ub_render_default_env(void)
{
    ub_render_env e;
    e.sun_dir = v3_normalize(v3(-0.5f, 0.78f, -0.38f)); /* high, front-left: shadows fall to the right */
    e.sun_strength = 0.62f;
    e.ambient = 0.38f;
    e.sky_top = 0x3A6FD8;
    e.sky_horizon = 0xBFD8F2;
    e.ground_color = 0x55703F;
    e.sun_color = 0xFFF2DC;
    e.sky_light = 0x8FA8C8;
    e.ground_light = 0x5A5248;
    e.fog_density = 0.0035f;
    return e;
}

#endif
