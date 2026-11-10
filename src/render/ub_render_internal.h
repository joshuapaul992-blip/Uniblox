/* Uniblox - interface between the renderer front end and its backends. */
#ifndef UB_RENDER_INTERNAL_H
#define UB_RENDER_INTERNAL_H

#include "render/ub_render.h"

typedef struct {
    ub_render_image img;
    vec3 c[4];
    bool on_top;
    f32 brightness;
} ub_image_quad;

/* software backend (ub_render_sw.c) */
void sw_image_quad(const ub_image_quad *q);       /* after the parts, before sw_end */
void sw_overlay(ub_framebuffer *fb, const ub_render_image *img, const int *rects, int nrects);
bool sw_init(void);
void sw_shutdown(void);
void sw_begin(ub_framebuffer *fb, const ub_camera *cam, const ub_render_env *env);
void sw_part(const ub_part_draw *p);
void sw_end(ub_render_stats *stats);
/* the fast renderer: draws the scene kept since sw_begin (before the GUI pictures) */
void sw_flush(void);
void sw_set_fast(bool on, int threads);
void sw_set_quality(int q); /* fast renderer: 0 low, 1 medium, 2 high */
bool sw_fast(void);
int sw_threads(void);

/* OpenGL backend (ub_render_gl.c) */
bool gl_init(char *desc, int desc_len, bool fixed_function);
void gl_shutdown(void);
void gl_set_quality(ub_quality q);
void gl_set_static(const ub_part_draw *parts, int count);
void gl_draw_scene(int width, int height, const ub_camera *cam, const ub_render_env *env,
                   const ub_part_draw *parts, int count, const ub_image_quad *quads, int nquads,
                   ub_render_stats *stats);
void gl_overlay(const ub_render_image *img, const int *rects, int nrects);
void gl_begin_2d(int width, int height);
void gl_text(int x, int y, const char *utf8, u32 color);
int gl_line_height(void);
void gl_set_text_px(int px);
void gl_rect(int x, int y, int w, int h, u32 color);
void gl_clip(int x, int y, int w, int h);

#endif
