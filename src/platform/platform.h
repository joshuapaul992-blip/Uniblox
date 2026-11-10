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
    UB_BACKEND_SOFTWARE,   /* app draws into a ub_framebuffer; works everywhere */
    UB_BACKEND_OPENGL,     /* app draws with OpenGL; needs a driver (any GPU) */
} ub_backend;

typedef enum {
    UB_BACKEND_PREF_AUTO,  /* OpenGL with a real (non-software) driver, else software */
    UB_BACKEND_PREF_OPENGL,
    UB_BACKEND_PREF_SOFTWARE,
    UB_BACKEND_PREF_FAST,  /* the software backend's fast renderer (the app chooses it; auto falls back to it) */
} ub_backend_pref;

typedef enum {
    UB_PIXEL_XRGB32, /* 0x00RRGGBB per pixel; used on 15/16/24/32-bit displays */
    UB_PIXEL_INDEX8, /* palette index per pixel; used on 256-colour displays */
} ub_pixel_format;

/* Software framebuffer, top-down rows. Both buffers always exist; `format`
 * says which one is shown this frame. It is usually smaller than the
 * window and stretched to fit (see plat_set_sw_resolution). */
typedef struct {
    ub_pixel_format format;
    u32 *pixels;            /* XRGB32 buffer */
    u8 *pixels8;            /* INDEX8 buffer */
    int width;
    int height;
    int pitch;              /* in pixels, same for both buffers */
    const u32 *palette;     /* 256 x 0x00RRGGBB for INDEX8, set by the app */
    u32 palette_serial;     /* app increments this when palette contents change */
} ub_framebuffer;

#define UB_TEXT_QUEUE_MAX 128
#define UB_TEXT_ITEM_LEN 120

/* Text for the software backend, drawn by the OS at window resolution after
 * the frame is stretched, so it stays sharp. (The OpenGL renderer draws
 * text itself from a font atlas.) */
typedef struct {
    int x, y;               /* window pixels, top-left of the line */
    u32 color;              /* 0x00RRGGBB; bit 24 set = no drop shadow */
    char text[UB_TEXT_ITEM_LEN];
} ub_text_item;

/* Everything the app needs to know to draw one frame. */
typedef struct {
    ub_backend backend;
    int width, height;      /* window client area in pixels */
    ub_framebuffer *fb;     /* software backend only, else NULL */
    ub_text_item text[UB_TEXT_QUEUE_MAX]; /* software backend only */
    int text_count;
    int text_px;            /* font height for the text queue */
    bool fullscreen;
    u32 fps_cap;            /* 0 = unlimited */
    u32 present_us;         /* time to show the previous frame */
    char present_detail[80];/* software backend: where present_us went */
    u32 sleep_us;           /* time the frame limiter waited last frame */
} ub_frame;

/* Settings the platform needs before it creates the window, provided by
 * app_preinit() (usually from the app's config file). */
typedef struct {
    ub_backend_pref backend;
    int window_w, window_h; /* 0 = choose from the screen size */
    bool fullscreen;
    u32 fps_cap;            /* 0 = unlimited */
    bool vsync;
    int sw_height;          /* software backend: render height (0 = window height) */
    bool timer_qpc;         /* time with QueryPerformanceCounter even when the CPU's counter is steady */
    int fs_width, fs_height;/* fullscreen display mode (NT 4.0 and later); 0 = keep the desktop's */
} ub_display_prefs;

/* Choose the software pixel format from the next frame on. The default
 * follows the desktop colour depth (INDEX8 at 256 colours or fewer). */
void plat_set_pixel_format(ub_pixel_format f);

/* Software backend: render at this height (width follows the window's
 * aspect ratio) and stretch to the window. 0 = the window's own size. */
void plat_set_sw_resolution(int height);

/* Borderless fullscreen on the monitor holding the window, or back. */
void plat_set_fullscreen(bool on);

/* Fullscreen display modes (NT 4.0 and later: NT 3.51 cannot change them).
 * A smaller mode means fewer pixels to send each frame, and the monitor
 * does the stretching. */
typedef struct {
    int w, h;
} ub_display_mode;
/* The sizes the screen can switch to at its current colour depth, smallest
 * first; 0 when modes cannot be changed here. */
int plat_display_modes(ub_display_mode *out, int max);
/* The mode to use while fullscreen (0, 0 = keep the desktop's). */
void plat_set_fullscreen_mode(int w, int h);

/* Frame rate limit, 0 = unlimited. */
void plat_set_fps_cap(u32 fps);

/* Software backend: send the screen only the parts of the window that
 * changed since the last frame (default on). Writing to the graphics card
 * is slow on some systems (NT 3.51 with a VESA driver); comparing frames
 * in memory is not. */
void plat_set_present_changed_only(bool on);

/* OpenGL backend: address of a GL function (core 1.1 or extension), NULL
 * if the driver lacks it. */
void *plat_gl_proc(const char *name);

/* A rasterised UI font for the OpenGL renderer: the 96 characters
 * 32..127 in a 16 x 6 grid of cells, 8-bit coverage, top-down rows. */
typedef struct {
    u8 *pixels;             /* plat_alloc'd; free with plat_free */
    int width, height;      /* atlas size (powers of two) */
    int cell_w, cell_h;     /* grid spacing; glyphs are drawn 1 pixel in from the cell corner */
    int glyph_h;            /* height of the drawn glyph area */
    int line_h;             /* recommended line spacing */
    u8 advance[96];         /* horizontal advance per character */
} ub_font_atlas;

bool plat_font_atlas(int px_height, ub_font_atlas *out);

/* Fonts for the GUI (Roblox's TextLabels...). Uniblox ships no fonts: the
 * GUI's typefaces map onto the ones Windows has had since NT 3.1 (Arial,
 * Times New Roman, Courier New) and, where present, Comic Sans MS and Arial
 * Black. Glyphs are rasterised at 4x and averaged down, so they are
 * anti-aliased the same way on every Windows version. */
enum { UB_FACE_SANS, UB_FACE_SERIF, UB_FACE_MONO, UB_FACE_HAND, UB_FACE_FAMILIES };
typedef struct {
    u8 family;              /* UB_FACE_* */
    u8 weight;              /* 1 light, 4 regular, 7 bold, 9 heavy (Roblox's weight / 100) */
    u8 italic;
    u8 pad;
} ub_typeface;
/* Ascent and descent as fractions of the em. */
bool plat_face_metrics(ub_typeface f, f32 *ascent, f32 *descent);
/* Horizontal advance of a character, in ems. */
f32 plat_glyph_advance(ub_typeface f, u32 codepoint);
/* The glyph at `em_px` pixels per em: coverage 0..255 in out[w*h] (at
 * most `cap` bytes), placed with its top-left (x0, y0) pixels from the pen
 * position on the baseline (y0 is usually negative). False if it does not
 * fit or the platform cannot draw it; w = 0 for blank glyphs. */
bool plat_glyph(ub_typeface f, u32 codepoint, f32 em_px, u8 *out, int cap, int *x0, int *y0, int *w, int *h);

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
    UB_KEY_PAGEUP    = 0x21,
    UB_KEY_PAGEDOWN  = 0x22,
    UB_KEY_END       = 0x23,
    UB_KEY_HOME      = 0x24,
    UB_KEY_LEFT      = 0x25,
    UB_KEY_UP        = 0x26,
    UB_KEY_RIGHT     = 0x27,
    UB_KEY_DOWN      = 0x28,
    UB_KEY_INSERT    = 0x2D,
    UB_KEY_DELETE    = 0x2E,
    UB_KEY_F1        = 0x70,
    UB_KEY_F2        = 0x71,
    UB_KEY_F3        = 0x72,
    UB_KEY_F4        = 0x73,
    UB_KEY_F5        = 0x74,
    UB_KEY_F6        = 0x75,
    UB_KEY_F7        = 0x76,
    UB_KEY_F8        = 0x77,
    UB_KEY_F9        = 0x78,
    UB_KEY_F10       = 0x79,
    UB_KEY_F11       = 0x7A,
    UB_KEY_F12       = 0x7B,
};

enum { UB_MOUSE_LEFT, UB_MOUSE_RIGHT, UB_MOUSE_MIDDLE, UB_MOUSE_BUTTONS };

/* One wheel notch, as Windows reports it (WHEEL_DELTA). */
#define UB_WHEEL_NOTCH 120

#define UB_TEXT_INPUT_MAX 64

typedef struct {
    /* State, persists across frames. */
    bool key_down[UB_KEY_COUNT];
    bool mouse_down[UB_MOUSE_BUTTONS];
    int mouse_x, mouse_y;     /* window client coordinates (same as ub_frame.width/height) */
    bool mouse_captured;      /* see plat_set_mouse_capture */
    bool has_focus;
    bool quit_requested;

    /* Events since the previous frame, cleared every frame. A key that is
     * pressed and released within one frame shows up in both arrays.
     * Keyboard auto-repeat does not set key_pressed. */
    bool key_pressed[UB_KEY_COUNT];
    bool key_released[UB_KEY_COUNT];
    bool key_repeat[UB_KEY_COUNT];   /* pressed, or repeated by holding it (for text editing) */
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
    char cpu_desc[160];      /* e.g. "Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz (family 6 model 158), MMX, SSE2, ~3192 MHz" */
    bool invariant_tsc;      /* the time-stamp counter runs at one rate (Nehalem / Haswell and later) */
    u32 cpu_mhz;             /* measured, 0 if unknown */
    bool palette_display;    /* the display is palette-based (256 colours) */
    bool has_mouse_wheel;    /* the OS reports wheel events (NT 4.0+, or a driver) */
    int screen_w, screen_h;  /* primary monitor size in pixels */
    char gl_desc[96];        /* "vendor / renderer / version", empty without OpenGL */
    int cpu_count;           /* processors the OS runs us on */
} ub_system_info;

/* ---------------------------------------------------------- processors */

/* Processors the OS runs us on (at least 1). */
int plat_cpu_count(void);
/* Runs fn(ctx, job) for job = 0..count-1 over up to plat_cpu_count()
 * threads, the calling one included (job j runs on thread j % threads);
 * returns when every job is done. Only one call at a time. */
void plat_parallel(void (*fn)(void *ctx, int job), void *ctx, int count);

typedef struct {
    u16 year, month, day, hour, minute, second;
    u16 millisecond;
} ub_datetime;

/* ------------------------------------------------------- time, memory */

/* Monotonic time in microseconds. */
u64 plat_time_us(void);
void plat_local_time(ub_datetime *out);
void plat_utc_time(ub_datetime *out);      /* Coordinated Universal Time */

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
/* Create a folder (game path); true if it exists afterwards. */
bool plat_make_dir(const char *path);
/* Calls fn for each entry of a folder (game path; "" = the game's folder),
 * names in UTF-8, without "." and "..". False if the folder can't be read. */
typedef void (*ub_list_fn)(void *user, const char *name, bool is_dir, u32 size);
bool plat_list_files(const char *dir, ub_list_fn fn, void *user);

/* Read a file the *user* chose (command line, later a file dialog): any
 * absolute or relative path, UTF-8. Never use this for paths that come
 * from game content; those must go through the checked game-path API. */
bool plat_file_read_user(const char *path, u32 max_bytes, void **data, u32 *size);

/* The first command-line argument (quotes removed), UTF-8; false if none. */
bool plat_command_line_arg(char *out, int cap);

/* Window title, UTF-8. */
void plat_set_title(const char *title);

#endif
