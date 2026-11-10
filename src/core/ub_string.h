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

/* A number with up to `decimals` places, trailing zeros dropped
 * ("2", "0.5", "-1.25"). */
int ub_fmt_num(char *buf, int size, f32 v, int decimals);
/* Parses "12", "-0.5", "3.", ".25", "1e3" (surrounding spaces allowed).
 * False if it is not a number. */
bool ub_parse_num(const char *s, f32 *out);

#endif
