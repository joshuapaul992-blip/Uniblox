/* Uniblox - interface between the renderer front end and its backends. */
#ifndef UB_RENDER_INTERNAL_H
#define UB_RENDER_INTERNAL_H

#include "render/ub_render.h"

/* software backend (ub_render_sw.c) */
bool sw_init(void);
void sw_shutdown(void);
void sw_begin(ub_framebuffer *fb, const ub_camera *cam, const ub_render_env *env);
void sw_part(const ub_part_draw *p);
void sw_end(ub_render_stats *stats);

/* OpenGL backend (ub_render_gl.c) */
bool gl_init(char *desc, int desc_len, bool fixed_function);
void gl_shutdown(void);
void gl_set_quality(ub_quality q);
void gl_set_static(const ub_part_draw *parts, int count);
void gl_draw_scene(int width, int height, const ub_camera *cam, const ub_render_env *env,
                   const ub_part_draw *parts, int count, ub_render_stats *stats);
void gl_begin_2d(int width, int height);
void gl_text(int x, int y, const char *utf8, u32 color);
int gl_line_height(void);

#endif
