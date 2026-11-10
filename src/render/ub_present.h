/* Uniblox - putting only what changed on the screen.
 *
 * Writing to the graphics card is the slow part on some machines (NT 3.51
 * with a VESA driver writes the card's memory uncached: about 95 MB/s, so
 * a full 1440p frame takes 150 ms). Comparing two frames in normal memory
 * costs almost nothing in comparison, so the software renderer's output is
 * compared with the previous frame in tiles, and only the window
 * rectangles under changed tiles are stretched, redrawn and sent.
 *
 * A frame is `fw x fh` pixels and is shown in a `ww x wh` window
 * (stretched, nearest pixel). Window pixel (x, y) shows frame pixel
 * (x * fw / ww, y * fh / wh); a frame tile covers the window pixels from
 * ub_present_win_x(tile start) up to ub_present_win_x(tile end), so
 * partial updates match a full stretch exactly. */
#ifndef UB_PRESENT_H
#define UB_PRESENT_H

#include "core/ub_types.h"

#define UB_DAMAGE_TILE 16   /* frame pixels per tile side */

typedef struct {
    int x, y, w, h;
} ub_prect;

typedef struct {
    int fw, fh, ww, wh;     /* frame and window sizes */
    int tx, ty;             /* tiles across and down */
    u32 *prev;              /* the previous frame */
    u8 *dirty;              /* per tile */
    int dirty_count;
    bool all;               /* everything must be sent (first frame, resize, exposure) */
} ub_damage;

void ub_damage_free(ub_damage *d);
/* Size for a frame and window; true if anything changed (then all is dirty). */
bool ub_damage_resize(ub_damage *d, int fw, int fh, int ww, int wh);
void ub_damage_invalidate(ub_damage *d);
/* Compares the frame with the previous one (and keeps it); marks changed
 * tiles. Returns how many tiles are dirty. */
int ub_damage_update(ub_damage *d, const u32 *pixels, int pitch);
/* Also redraw this window rectangle (text drawn over the frame). */
void ub_damage_mark_window(ub_damage *d, int x, int y, int w, int h);
/* The dirty window rectangles, merged along tile rows (and with the row
 * below when they line up). Returns how many; at most max. */
int ub_damage_rects(const ub_damage *d, ub_prect *out, int max);

int ub_present_win_x(const ub_damage *d, int frame_x);  /* first window column showing frame column x */
int ub_present_win_y(const ub_damage *d, int frame_y);

/* Nearest-pixel stretch of the frame into part of a window-sized 32-bit
 * image (rows top-down when dst_pitch > 0; pass a negative pitch and the
 * last row for bottom-up images). */
void ub_present_stretch(const u32 *src, int fw, int fh, int spitch, u32 *dst, int ww, int wh, int dst_pitch,
                        ub_prect r);

#endif
