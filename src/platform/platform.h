/* Uniblox - platform layer interface.
 *
 * Everything above this header (app, render, world, physics, script, net)
 * is OS-independent C99. Each OS backend (win32 today, POSIX later for the
 * dedicated server) implements these functions and drives the app_* calls
 * declared in app/app.h. */
#ifndef UB_PLATFORM_H
#define UB_PLATFORM_H

#include "core/ub_types.h"

/* Software framebuffer the app renders into: 0x00RRGGBB, top-down rows.
 * The backend converts it to whatever the display needs. */
typedef struct {
    u32 *pixels;
    int width;
    int height;
    int pitch; /* in pixels */
} ub_framebuffer;

enum { UB_KEY_COUNT = 256 };

/* Key codes match Win32 virtual-key codes; other backends translate. */
enum {
    UB_KEY_ESCAPE = 0x1B,
    UB_KEY_SPACE  = 0x20,
    UB_KEY_LEFT   = 0x25,
    UB_KEY_UP     = 0x26,
    UB_KEY_RIGHT  = 0x27,
    UB_KEY_DOWN   = 0x28,
};

typedef struct {
    bool key_down[UB_KEY_COUNT];
    int mouse_x, mouse_y; /* framebuffer coordinates */
    bool mouse_left, mouse_right;
    bool quit_requested;
} ub_input;

#define UB_OVERLAY_MAX_LINES 8
#define UB_OVERLAY_LINE_LEN 96

/* Text drawn on top of the frame by the backend (until we have our own
 * font renderer). ASCII/UTF-8. */
typedef struct {
    char lines[UB_OVERLAY_MAX_LINES][UB_OVERLAY_LINE_LEN];
    int count;
} ub_overlay;

/* Facts about the machine, filled in by the backend at startup. */
typedef struct {
    char os_name[64];        /* e.g. "Windows NT 3.51 build 1057" */
    u32 os_major, os_minor, os_build;
    u64 total_phys_bytes;    /* installed RAM as reported by the OS */
    int display_bpp;         /* desktop colour depth */
} ub_system_info;

/* Monotonic time in microseconds. */
u64 plat_time_us(void);

void *plat_alloc(size_t bytes);  /* zeroed */
void plat_free(void *p);

/* Debug log (OutputDebugString on Win32). */
void plat_log(const char *fmt, ...);

#endif
