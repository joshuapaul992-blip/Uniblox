/* Uniblox - UTF-8 <-> UTF-16 conversion.
 * The engine uses UTF-8 everywhere; Win32 "W" APIs want UTF-16. NT 3.51
 * has no CP_UTF8 in MultiByteToWideChar, so we convert ourselves. */
#ifndef UB_UTF_H
#define UB_UTF_H

#include "core/ub_types.h"

/* Decode one code point from UTF-8; advances *s. An invalid lead or
 * continuation byte yields U+FFFD and consumes one byte; a well-formed but
 * invalid sequence (overlong, surrogate, > U+10FFFF) yields one U+FFFD for
 * the whole sequence. Returns 0 at the terminating NUL. */
u32 ub_utf8_decode(const char **s);

/* Encode one code point as UTF-8 into out (needs 4 bytes). Returns bytes
 * written; invalid code points (surrogates, > U+10FFFF) become U+FFFD. */
int ub_utf8_encode(u32 cp, char *out);

/* NUL-terminated UTF-8 -> UTF-16, surrogate pairs for > U+FFFF.
 * Truncates to fit; returns code units written (excluding NUL). */
int ub_utf8_to_utf16(const char *src, u16 *dst, int cap);

/* NUL-terminated UTF-16 -> UTF-8. Truncates on a whole-character boundary.
 * Returns bytes written (excluding NUL). */
int ub_utf16_to_utf8(const u16 *src, char *dst, int cap);

#endif
