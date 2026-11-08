/* Uniblox - the fixed 256-colour palette used in 8-bit display modes.
 *
 * Windows reserves palette entries 0-9 and 246-255 for its 20 static
 * system colours, so Uniblox uses only 10..245 (236 colours):
 *   10..225  a 6x6x6 colour cube (levels 0, 51, 102, 153, 204, 255)
 *   226..245 20 extra greys between the cube's six greys
 * The reserved entries are left black here; the platform fills them with
 * the real system colours when it builds a Windows palette. */
#ifndef UB_PALETTE_H
#define UB_PALETTE_H

#include "core/ub_types.h"

#define UB_PAL_FIRST 10
#define UB_PAL_CUBE  10
#define UB_PAL_GRAY  226
#define UB_PAL_LAST  245

/* 256 entries, 0x00RRGGBB. Valid after ub_palette_init(). */
extern u32 ub_palette[256];

void ub_palette_init(void);

/* Nearest palette index for an RGB colour (each 0..255). dither is a
 * 2x2 ordered-dither cell 0..3 ((x & 1) | (y & 1) << 1), or -1 for none. */
u8 ub_palette_match(int r, int g, int b, int dither);

#endif
