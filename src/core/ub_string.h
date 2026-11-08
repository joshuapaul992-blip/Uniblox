/* Uniblox - string helpers (no libc available). */
#ifndef UB_STRING_H
#define UB_STRING_H

#include <stdarg.h>
#include "core/ub_types.h"

size_t ub_strlen(const char *s);

/* Tiny snprintf subset: %d %i %u %x %X %s %c %%, optional zero-pad and
 * width (e.g. %08x, %5d), and the 'l' length modifier (ignored: int and long
 * are both 32-bit on Win32). Always NUL-terminates when size > 0.
 * Returns the number of characters written, excluding the NUL. */
int ub_vfmt(char *buf, int size, const char *fmt, va_list ap);
int ub_fmt(char *buf, int size, const char *fmt, ...);

#endif
