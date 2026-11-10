/* Uniblox - app entry points called by the platform backend.
 *
 * Order: app_preinit (before any window exists) -> app_init -> app_frame
 * every frame -> app_shutdown. */
#ifndef UB_APP_H
#define UB_APP_H

#include "platform/platform.h"

/* Default window size before the screen size is known. */
#define UB_DEFAULT_WINDOW_W 640
#define UB_DEFAULT_WINDOW_H 480

/* Minimum / recommended installed RAM (project requirement). */
#define UB_RAM_MIN_MB 384u
#define UB_RAM_REC_MB 512u

/* Who is running: the window title and log file names. */
typedef struct {
    const char *title;          /* window title and message boxes */
    const char *log_path;       /* e.g. "uniblox.log" */
    const char *old_log_path;   /* the previous run's log */
} ub_app_info;
const ub_app_info *app_info(void);

/* Fill in display settings (renderer, window size, fps cap...). */
void app_preinit(ub_display_prefs *prefs);
bool app_init(const ub_system_info *sys, const ub_frame *frame);
/* dt_us: microseconds since the previous frame. */
void app_frame(ub_frame *frame, const ub_input *in, u32 dt_us);
void app_shutdown(void);

#endif
