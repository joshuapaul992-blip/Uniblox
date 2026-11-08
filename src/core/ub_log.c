#include "core/ub_log.h"
#include "core/ub_string.h"
#include "platform/platform.h"

static ub_file *g_file;
static ub_mutex *g_lock;
static u64 g_start_us;

void ub_log_open(const char *path, const char *old_path)
{
    if (!g_lock) g_lock = plat_mutex_create();
    g_start_us = plat_time_us();
    if (old_path && plat_file_exists(path)) plat_file_rename(path, old_path);
    g_file = plat_file_open(path, UB_FILE_WRITE);

    ub_datetime t;
    plat_local_time(&t);
    ub_info("Uniblox log started %04u-%02u-%02u %02u:%02u:%02u", t.year, t.month,
            t.day, t.hour, t.minute, t.second);
    if (!g_file) ub_warn("could not create log file %s; logging to debugger only", path);
}

void ub_log_close(void)
{
    if (g_lock) plat_mutex_lock(g_lock);
    if (g_file) {
        plat_file_close(g_file);
        g_file = NULL;
    }
    if (g_lock) plat_mutex_unlock(g_lock);
}

bool ub_log_is_file_open(void)
{
    return g_file != NULL;
}

void ub_logv(ub_log_level level, const char *fmt, va_list ap)
{
    static const char *const names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    char line[600];
    u32 ms = (u32)((plat_time_us() - g_start_us) / 1000u);
    int n = ub_fmt(line, sizeof line, "[%5u.%03u] %s ", ms / 1000u, ms % 1000u,
                   names[level & 3]);
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    n += ub_vfmt(line + n, (int)sizeof line - n - 2, fmt, ap);
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n'; /* CRLF so Notepad on NT 3.51 shows it properly */
    line[n] = '\0';

    if (g_lock) plat_mutex_lock(g_lock);
    plat_debug_output(line);
    if (g_file) plat_file_write(g_file, line, (u32)n);
    if (g_lock) plat_mutex_unlock(g_lock);
}

void ub_log(ub_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ub_logv(level, fmt, ap);
    va_end(ap);
}
