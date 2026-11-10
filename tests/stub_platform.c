/* Uniblox - minimal platform layer for host (Linux) unit tests.
 * Single-threaded; files always fail; memory comes from libc. */
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <time.h>
#include "platform/platform.h"

/* A real clock: the script time limit is measured with it. */
u64 plat_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000u + (u64)t.tv_nsec / 1000u;
}

void plat_local_time(ub_datetime *out)
{
    ub_datetime t = { 2000, 1, 1, 0, 0, 0 };
    *out = t;
}

void *plat_alloc(size_t bytes) { return calloc(1, bytes ? bytes : 1); }
void plat_free(void *p) { free(p); }
void plat_debug_output(const char *text) { (void)text; }

struct ub_mutex { int depth; };
ub_mutex *plat_mutex_create(void) { return (ub_mutex *)calloc(1, sizeof(ub_mutex)); }
void plat_mutex_destroy(ub_mutex *m) { free(m); }
void plat_mutex_lock(ub_mutex *m) { m->depth++; }
void plat_mutex_unlock(ub_mutex *m) { m->depth--; }

ub_file *plat_file_open(const char *path, ub_file_mode mode) { (void)path; (void)mode; return NULL; }
i32 plat_file_read(ub_file *f, void *buf, u32 bytes) { (void)f; (void)buf; (void)bytes; return -1; }
bool plat_file_write(ub_file *f, const void *buf, u32 bytes) { (void)f; (void)buf; (void)bytes; return false; }
u32 plat_file_size(ub_file *f) { (void)f; return 0; }
void plat_file_close(ub_file *f) { (void)f; }
bool plat_file_read_all(const char *p, u32 m, void **d, u32 *s) { (void)p; (void)m; *d = NULL; *s = 0; return false; }
bool plat_file_write_all(const char *p, const void *d, u32 s) { (void)p; (void)d; (void)s; return false; }
bool plat_file_rename(const char *a, const char *b) { (void)a; (void)b; return false; }
bool plat_file_exists(const char *p) { (void)p; return false; }
void plat_set_mouse_capture(bool on) { (void)on; }
void plat_request_quit(void) {}
void plat_set_pixel_format(ub_pixel_format f) { (void)f; }
void plat_set_sw_resolution(int height) { (void)height; }
void plat_set_fullscreen(bool on) { (void)on; }
void plat_set_fps_cap(u32 fps) { (void)fps; }
void *plat_gl_proc(const char *name) { (void)name; return NULL; }
bool plat_font_atlas(int px, ub_font_atlas *out) { (void)px; (void)out; return false; }
bool plat_make_dir(const char *p) { (void)p; return false; }
bool plat_file_read_user(const char *p, u32 m, void **d, u32 *s) { (void)p; (void)m; *d = NULL; *s = 0; return false; }
bool plat_command_line_arg(char *out, int cap) { (void)out; (void)cap; return false; }
void plat_set_title(const char *t) { (void)t; }
