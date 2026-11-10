/* Uniblox - Roblox XML places and models (see ub_rbxlx.c). */
#ifndef UB_RBXLX_H
#define UB_RBXLX_H

#include "core/ub_types.h"

bool ub_rbxlx_detect(const u8 *data, u32 size);
/* The XML as a binary .rbxm in memory (plat_free *out_data). */
bool ub_rbxlx_to_binary(const u8 *data, u32 size, u8 **out_data, u32 *out_size);

#endif
