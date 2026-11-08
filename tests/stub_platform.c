/* Uniblox - minimal platform layer for host (Linux) unit tests.
 * Single-threaded; files always fail; memory comes from libc. */
#include <stdlib.h>
#include "platform/platform.h"

u64 plat_time_us(void) { return 0; }

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
