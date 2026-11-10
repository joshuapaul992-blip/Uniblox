/* Uniblox - minimal platform layer for host (Linux) unit tests.
 * Single-threaded; memory comes from libc. Game files live under
 * build/host/files/ (so the editor's tests can save and open places). */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "core/ub_path.h"
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
    ub_datetime t = { 2000, 1, 1, 0, 0, 0, 0 };
    *out = t;
}

/* Fixed too (tests compare dates): the local time is one hour ahead. */
void plat_utc_time(ub_datetime *out)
{
    ub_datetime t = { 1999, 12, 31, 23, 0, 0, 0 };
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

void plat_set_mouse_capture(bool on) { (void)on; }
void plat_request_quit(void) {}
void plat_set_pixel_format(ub_pixel_format f) { (void)f; }
void plat_set_sw_resolution(int height) { (void)height; }
void plat_set_fullscreen(bool on) { (void)on; }
void plat_set_fps_cap(u32 fps) { (void)fps; }
void plat_set_present_changed_only(bool on) { (void)on; }
int plat_display_modes(ub_display_mode *out, int max)
{
    static const ub_display_mode modes[] = { { 640, 480 }, { 800, 600 }, { 1280, 720 }, { 1920, 1080 } };
    int n = 0;
    for (; n < 4 && n < max; ++n) out[n] = modes[n];
    return n;
}
void plat_set_fullscreen_mode(int w, int h) { (void)w; (void)h; }
void *plat_gl_proc(const char *name) { (void)name; return NULL; }
bool plat_font_atlas(int px, ub_font_atlas *out) { (void)px; (void)out; return false; }

/* GUI fonts for the tests: every character is a box half an em wide
 * (spaces a quarter), ascent 0.8 em, descent 0.2 em, so layouts can be
 * checked exactly. Bold boxes are a little wider. */
bool plat_face_metrics(ub_typeface f, f32 *ascent, f32 *descent)
{
    (void)f;
    *ascent = 0.8f;
    *descent = 0.2f;
    return true;
}

f32 plat_glyph_advance(ub_typeface f, u32 cp)
{
    if (cp == ' ') return 0.25f;
    return f.weight >= 7 ? 0.625f : 0.5f;
}

bool plat_glyph(ub_typeface f, u32 cp, f32 em_px, u8 *out, int cap, int *x0, int *y0, int *w, int *h)
{
    *x0 = *y0 = *w = *h = 0;
    if (cp == ' ' || cp == '\n') return true;
    int gw = (int)(em_px * (f.weight >= 7 ? 0.5f : 0.4f)), gh = (int)(em_px * 0.6f);
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (gw * gh > cap) return false;
    for (int i = 0; i < gw * gh; ++i) out[i] = 255;
    *x0 = (int)(em_px * 0.05f);
    *y0 = -gh;
    *w = gw;
    *h = gh;
    return true;
}
bool plat_command_line_arg(char *out, int cap) { (void)out; (void)cap; return false; }
void plat_set_title(const char *t) { (void)t; }

#define FILES_ROOT "build/host/files"

static bool full_path(const char *path, char *out, size_t cap)
{
    if (path[0] && !ub_path_is_safe(path)) return false;
    mkdir("build", 0755);
    mkdir("build/host", 0755);
    mkdir(FILES_ROOT, 0755);
    snprintf(out, cap, "%s%s%s", FILES_ROOT, path[0] ? "/" : "", path);
    return true;
}

struct ub_file { FILE *f; };

ub_file *plat_file_open(const char *path, ub_file_mode mode)
{
    char full[600];
    if (!full_path(path, full, sizeof full)) return NULL;
    FILE *f = fopen(full, mode == UB_FILE_READ ? "rb" : mode == UB_FILE_WRITE ? "wb" : "ab");
    if (!f) return NULL;
    ub_file *u = (ub_file *)calloc(1, sizeof *u);
    u->f = f;
    return u;
}
i32 plat_file_read(ub_file *f, void *buf, u32 bytes) { return (i32)fread(buf, 1, bytes, f->f); }
bool plat_file_write(ub_file *f, const void *buf, u32 bytes) { return fwrite(buf, 1, bytes, f->f) == bytes; }
u32 plat_file_size(ub_file *f)
{
    long at = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long n = ftell(f->f);
    fseek(f->f, at, SEEK_SET);
    return (u32)n;
}
void plat_file_close(ub_file *f)
{
    if (!f) return;
    fclose(f->f);
    free(f);
}

static bool read_whole(const char *full, u32 max, void **data, u32 *size)
{
    *data = NULL;
    *size = 0;
    FILE *f = fopen(full, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (u32)n > max) {
        fclose(f);
        return false;
    }
    char *b = (char *)calloc(1, (size_t)n + 1);
    bool ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(b);
        return false;
    }
    *data = b;
    *size = (u32)n;
    return true;
}

bool plat_file_read_all(const char *path, u32 max, void **data, u32 *size)
{
    char full[600];
    *data = NULL;
    *size = 0;
    return full_path(path, full, sizeof full) && read_whole(full, max, data, size);
}

bool plat_file_read_user(const char *path, u32 max, void **data, u32 *size) { return read_whole(path, max, data, size); }

bool plat_file_write_all(const char *path, const void *data, u32 size)
{
    char full[600];
    if (!full_path(path, full, sizeof full)) return false;
    FILE *f = fopen(full, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool plat_file_rename(const char *a, const char *b)
{
    char fa[600], fb[600];
    return full_path(a, fa, sizeof fa) && full_path(b, fb, sizeof fb) && rename(fa, fb) == 0;
}

bool plat_file_exists(const char *path)
{
    char full[600];
    struct stat st;
    return full_path(path, full, sizeof full) && stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

bool plat_make_dir(const char *path)
{
    char full[600];
    struct stat st;
    if (!full_path(path, full, sizeof full)) return false;
    mkdir(full, 0755);
    return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

bool plat_list_files(const char *dir, ub_list_fn fn, void *user)
{
    char full[600];
    if (!full_path(dir, full, sizeof full)) return false;
    DIR *d = opendir(full);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1200];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", full, e->d_name);
        if (stat(p, &st) != 0) continue;
        fn(user, e->d_name, S_ISDIR(st.st_mode), (u32)st.st_size);
    }
    closedir(d);
    return true;
}
