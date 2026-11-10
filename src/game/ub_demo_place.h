/* Uniblox - the built-in demo place: a walled room with a scripted door, a
 * spinner, loose blocks and balls, a cart, a lava strip and a button that
 * rains balls. Its scripts are Roblox-style Lua (see ub_demo_place.c). */
#ifndef UB_DEMO_PLACE_H
#define UB_DEMO_PLACE_H

#include "world/ub_world.h"

/* Builds it in `w` (which is initialised first). */
void ub_demo_place(ub_world *w);

#endif
