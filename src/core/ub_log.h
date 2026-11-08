/* Uniblox - log file.
 *
 * Writes "uniblox.log" next to the executable (the previous run's log is
 * kept as "uniblox.old.log") and mirrors every line to the debugger.
 * If the folder is read-only (e.g. a CD-ROM) logging continues to the
 * debugger only. Safe to call from any thread. */
#ifndef UB_LOG_H
#define UB_LOG_H

#include <stdarg.h>
#include "core/ub_types.h"

typedef enum { UB_LOG_DEBUG, UB_LOG_INFO, UB_LOG_WARN, UB_LOG_ERROR } ub_log_level;

void ub_log_open(const char *path, const char *old_path);
void ub_log_close(void);
bool ub_log_is_file_open(void);

void ub_logv(ub_log_level level, const char *fmt, va_list ap);
void ub_log(ub_log_level level, const char *fmt, ...);

#define ub_debug(...) ub_log(UB_LOG_DEBUG, __VA_ARGS__)
#define ub_info(...)  ub_log(UB_LOG_INFO, __VA_ARGS__)
#define ub_warn(...)  ub_log(UB_LOG_WARN, __VA_ARGS__)
#define ub_error(...) ub_log(UB_LOG_ERROR, __VA_ARGS__)

#endif
