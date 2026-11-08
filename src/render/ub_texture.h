/* Uniblox - surface materials and their procedural textures.
 *
 * Every texture is 64x64 and stores brightness only, 0..31 (5 bits), so
 * any part colour can use any material: the renderer turns brightness into
 * a final colour through a small per-face lookup table. All textures tile. */
#ifndef UB_TEXTURE_H
#define UB_TEXTURE_H

#include "core/ub_types.h"

#define UB_TEX_SIZE  64
#define UB_TEX_SHIFT 6
#define UB_TEX_MASK  63
#define UB_TEX_LEVELS 32

typedef enum {
    UB_MAT_PLASTIC,  /* smooth: no texture, drawn with the fast flat path */
    UB_MAT_GRID,     /* light panels with grid lines, for baseplates */
    UB_MAT_BRICK,
    UB_MAT_WOOD,
    UB_MAT_GRASS,
    UB_MAT_METAL,
    UB_MAT_STONE,
    UB_MAT_COUNT
} ub_material;

typedef struct {
    const char *name;
    f32 world_size;   /* world units covered by one repeat of the texture */
} ub_material_info;

extern const ub_material_info ub_materials[UB_MAT_COUNT];

void ub_textures_init(void);

/* 64*64 texels, row-major, values 0..31. NULL for UB_MAT_PLASTIC. */
const u8 *ub_texture(ub_material m);

/* Brightness multiplier for a texel value, in 1/256ths (about 0.6x..1.15x). */
static inline int ub_texel_gain(int t) { return 154 + t * 140 / 31; }

#endif
