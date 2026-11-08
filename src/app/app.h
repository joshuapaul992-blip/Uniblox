/* Uniblox - app entry points called by the platform backend. */
#ifndef UB_APP_H
#define UB_APP_H

#include "platform/platform.h"

/* Internal render resolution. Kept small so the software renderer stays
 * fast on Pentium-class CPUs; the backend scales it to the window. */
#define UB_FB_WIDTH  320
#define UB_FB_HEIGHT 240

/* Minimum / recommended installed RAM (project requirement). */
#define UB_RAM_MIN_MB 384u
#define UB_RAM_REC_MB 512u

bool app_init(const ub_system_info *sys);
/* dt_us: microseconds since the previous frame. */
void app_frame(ub_framebuffer *fb, const ub_input *in, u32 dt_us, ub_overlay *ov);
void app_shutdown(void);

#endif
