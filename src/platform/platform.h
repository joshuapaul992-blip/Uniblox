/* Uniblox - platform layer interface.
 *
 * Everything above this header (app, audio mixer, and later render, world,
 * physics, script, net) is OS-independent C99. Each OS backend (win32
 * today, POSIX later for the dedicated server) implements these functions
 * and drives the app_* calls declared in app/app.h. */
#ifndef UB_PLATFORM_H
#define UB_PLATFORM_H

#include "core/ub_types.h"

/* ------------------------------------------------------------ display */

typedef enum {
    UB_PIXEL_XRGB32, /* 0x00RRGGBB per pixel; used on 15/16/24/32-bit displays */
    UB_PIXEL_INDEX8, /* palette index per pixel; used on 256-colour displays */
} ub_pixel_format;

/* Software framebuffer the app renders into, top-down rows. Both buffers
 * always exist; `format` says which one is shown this frame. The backend
 * converts to whatever the display needs. */
typedef struct {
    ub_pixel_format format;
    u32 *pixels;            /* XRGB32 buffer */
    u8 *pixels8;            /* INDEX8 buffer */
    int width;
    int height;
    int pitch;              /* in pixels, same for both buffers */
    const u32 *palette;     /* 256 x 0x00RRGGBB for INDEX8, set by the app */
    u32 palette_serial;     /* app increments this when palette contents change */
    u32 present_us;         /* backend: time to convert + show the previous frame */
    int window_scale;       /* backend: current window size as a multiple of width/height */
} ub_framebuffer;

/* Choose the pixel format from the next frame on. The backend picks a
 * default from the desktop colour depth (INDEX8 at 256 colours or fewer). */
void plat_set_pixel_format(ub_pixel_format f);

/* Resize the window to scale x the framebuffer (1..3). At 1 the image is
 * copied to the screen without stretching, which is much cheaper on slow
 * machines. */
void plat_set_window_scale(int scale);

#define UB_OVERLAY_MAX_LINES 10
#define UB_OVERLAY_LINE_LEN 96

/* Text drawn on top of the frame by the backend (until we have our own
 * font renderer). UTF-8. */
typedef struct {
    char lines[UB_OVERLAY_MAX_LINES][UB_OVERLAY_LINE_LEN];
    int count;
} ub_overlay;

/* -------------------------------------------------------------- input */

enum { UB_KEY_COUNT = 256 };

/* Key codes match Win32 virtual-key codes; other backends translate.
 * Letters and digits use their ASCII upper-case value ('A', '0'). */
enum {
    UB_KEY_BACKSPACE = 0x08,
    UB_KEY_TAB       = 0x09,
    UB_KEY_ENTER     = 0x0D,
    UB_KEY_SHIFT     = 0x10,
    UB_KEY_CONTROL   = 0x11,
    UB_KEY_ALT       = 0x12,
    UB_KEY_ESCAPE    = 0x1B,
    UB_KEY_SPACE     = 0x20,
    UB_KEY_LEFT      = 0x25,
    UB_KEY_UP        = 0x26,
    UB_KEY_RIGHT     = 0x27,
    UB_KEY_DOWN      = 0x28,
    UB_KEY_F1        = 0x70,
    UB_KEY_F2        = 0x71,
    UB_KEY_F3        = 0x72,
    UB_KEY_F5        = 0x74,
    UB_KEY_F6        = 0x75,
    UB_KEY_F8        = 0x77,
};

enum { UB_MOUSE_LEFT, UB_MOUSE_RIGHT, UB_MOUSE_MIDDLE, UB_MOUSE_BUTTONS };

/* One wheel notch, as Windows reports it (WHEEL_DELTA). */
#define UB_WHEEL_NOTCH 120

#define UB_TEXT_INPUT_MAX 64

typedef struct {
    /* State, persists across frames. */
    bool key_down[UB_KEY_COUNT];
    bool mouse_down[UB_MOUSE_BUTTONS];
    int mouse_x, mouse_y;     /* framebuffer coordinates */
    bool mouse_captured;      /* see plat_set_mouse_capture */
    bool has_focus;
    bool quit_requested;

    /* Events since the previous frame, cleared every frame. A key that is
     * pressed and released within one frame shows up in both arrays.
     * Keyboard auto-repeat does not set key_pressed. */
    bool key_pressed[UB_KEY_COUNT];
    bool key_released[UB_KEY_COUNT];
    bool mouse_pressed[UB_MOUSE_BUTTONS];
    bool mouse_released[UB_MOUSE_BUTTONS];
    int mouse_dx, mouse_dy;   /* relative motion in screen pixels, only while captured */
    int wheel;                /* multiples of UB_WHEEL_NOTCH; positive = away from user */
    /* Characters typed, in order, UTF-8, NUL-terminated. Besides printable
     * text it contains '\b' (Backspace), '\r' (Enter) and '\x1b' (Esc), so
     * a text box sees exactly the order the keys were pressed even when
     * several arrive within one frame. */
    char text[UB_TEXT_INPUT_MAX];
} ub_input;

/* Mouse-look mode: hides the cursor, keeps it inside the window, and
 * reports motion in mouse_dx/mouse_dy instead of moving the pointer.
 * Released automatically when the window loses focus. */
void plat_set_mouse_capture(bool on);

/* Ask the backend to close the game after the current frame (the window's
 * close button and Alt+F4 also set ub_input.quit_requested). */
void plat_request_quit(void);

/* ---------------------------------------------------------- system info */

typedef struct {
    char os_name[64];        /* e.g. "Windows NT 3.51 build 1057" */
    u32 os_major, os_minor, os_build;
    u64 total_phys_bytes;    /* installed RAM as reported by the OS */
    int display_bpp;         /* desktop colour depth */
    char audio_desc[64];     /* e.g. "22050 Hz 16-bit stereo" or "no audio device" */
    char cpu_desc[64];       /* e.g. "GenuineIntel family 5 model 2, MMX, ~133 MHz" */
    u32 cpu_mhz;             /* measured, 0 if unknown */
    bool palette_display;    /* the display is palette-based (256 colours) */
    bool has_mouse_wheel;    /* the OS reports wheel events (NT 4.0+, or a driver) */
} ub_system_info;

typedef struct {
    u16 year, month, day, hour, minute, second;
} ub_datetime;

/* ------------------------------------------------------- time, memory */

/* Monotonic time in microseconds. */
u64 plat_time_us(void);
void plat_local_time(ub_datetime *out);

void *plat_alloc(size_t bytes);  /* zeroed; NULL on failure */
void plat_free(void *p);

/* Text to the debugger (OutputDebugString on Win32). Use ub_log normally. */
void plat_debug_output(const char *text);

/* ------------------------------------------------------------ threads */

typedef struct ub_mutex ub_mutex;
ub_mutex *plat_mutex_create(void);
void plat_mutex_destroy(ub_mutex *m);
void plat_mutex_lock(ub_mutex *m);
void plat_mutex_unlock(ub_mutex *m);

/* -------------------------------------------------------------- files */

/* Paths are relative to the folder holding the executable, use '/', and
 * must pass ub_path_is_safe(); unsafe paths fail without touching the OS. */

typedef struct ub_file ub_file;

typedef enum {
    UB_FILE_READ,    /* existing file, read only */
    UB_FILE_WRITE,   /* create or truncate */
    UB_FILE_APPEND,  /* create if missing, write at end */
} ub_file_mode;

ub_file *plat_file_open(const char *path, ub_file_mode mode);
/* Returns bytes read (0 at end of file), or -1 on error. */
i32 plat_file_read(ub_file *f, void *buf, u32 bytes);
bool plat_file_write(ub_file *f, const void *buf, u32 bytes);
u32 plat_file_size(ub_file *f);
void plat_file_close(ub_file *f);

/* Reads a whole file into a plat_alloc'd buffer with a NUL appended (not
 * counted in *size). Refuses files larger than max_bytes. */
bool plat_file_read_all(const char *path, u32 max_bytes, void **data, u32 *size);
/* Writes to "<path>.tmp" then renames over path, so a crash mid-write
 * never leaves a half-written file. */
bool plat_file_write_all(const char *path, const void *data, u32 size);
/* Rename, replacing new_path if it exists. */
bool plat_file_rename(const char *old_path, const char *new_path);
bool plat_file_exists(const char *path);

#endif
