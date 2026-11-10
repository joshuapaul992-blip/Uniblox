/* Uniblox - pictures for ImageLabels, ImageButtons and Decals-to-come.
 *
 * Uniblox never downloads anything. A place's images are looked up in the
 * `content` folder next to uniblox.exe, where players put their own:
 *   rbxassetid://123 (and roblox.com asset links)  -> content\123.png / .bmp
 *   rbxasset://textures/x.png                      -> content\textures\x.png
 * PNG (every colour type and bit depth, interlaced or not) and BMP (8, 24
 * and 32-bit) are read. A missing picture draws nothing, as an image that
 * is still loading does in Roblox. */
#ifndef UB_IMAGE_H
#define UB_IMAGE_H

#include "gui/ub_canvas.h"

#define UB_IMAGE_MAX_SIDE 4096

/* Decodes a PNG or BMP into premultiplied 0xAARRGGBB (out->px is
 * plat_alloc'd). Untrusted input: false for anything malformed. */
bool ub_image_decode(const u8 *data, u32 size, ub_image_px *out);

/* The local file a content id names ("" if none); true if it has one. */
bool ub_image_path(const char *content, char *path, int cap, const char *ext);

/* The picture for a content id, loaded on first use and kept; NULL if
 * there is none (logged once). */
const ub_image_px *ub_image_get(const char *content);
void ub_image_flush(void);

#endif
