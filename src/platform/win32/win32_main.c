/* Uniblox - Win32 window, input and main loop.
 *
 * Display: the window is resizable, can go borderless fullscreen, and is
 * DPI-aware on Windows Vista and later (so 1440p/4K monitors get real
 * pixels instead of a blurry upscale). Rendering goes through OpenGL when
 * a driver is available, otherwise through the software renderer
 * (win32_display.c). Frames are paced to a configurable cap, 120 fps by
 * default.
 *
 * Input: keyboard via WM_KEYDOWN/WM_KEYUP/WM_CHAR, mouse via the button
 * and WM_MOUSEMOVE messages, in window client pixels. Mouse-look
 * re-centres the cursor every frame and reports the offset, the only
 * relative-mouse method that works on NT 3.51 (Raw Input is XP+). The
 * wheel arrives as WM_MOUSEWHEEL (NT 4.0+) or the registered
 * "MSWHEEL_ROLLMSG" message that older IntelliPoint drivers send. */
#include "platform/win32/win32_internal.h"
#include "app/app.h"
#include "audio/ub_mixer.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

#include <cpuid.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef SM_MOUSEWHEELPRESENT
#define SM_MOUSEWHEELPRESENT 75
#endif

#define WINDOWED_STYLE (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
#define FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE)

static HWND g_hwnd;
static HINSTANCE g_inst;
static ub_input g_input;
static ub_frame g_frame;
static UINT g_msh_wheel_msg; /* registered wheel message, 0 if unavailable */
static int g_text_len;
static u16 g_high_surrogate;
static bool g_look;          /* mouse-look active */
static POINT g_look_restore; /* cursor position to restore afterwards */
static RECT g_windowed_rect; /* window position before going fullscreen */
static bool g_want_fullscreen;
static u64 g_next_frame_us;

/* ---------------------------------------------------------- system info */

#define read_tsc win32_read_tsc
static u64 g_tsc_khz; /* measured */

/* CPUID (Pentium and later; __get_cpuid checks the instruction exists) and
 * a rough clock speed from the time-stamp counter over 50 ms. In a VM or
 * emulator the speed is what the guest sees, which is what matters for
 * judging benchmark results. */
static void query_cpu(ub_system_info *sys)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(0, &a, &b, &c, &d)) {
        ub_fmt(sys->cpu_desc, sizeof sys->cpu_desc, "386/486 class (no CPUID)");
        return;
    }
    char vendor[13];
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    unsigned family = 0, model = 0;
    bool mmx = false, sse = false, sse2 = false, tsc = false, avx2 = false;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        family = (a >> 8) & 15;
        model = (a >> 4) & 15;
        if (family == 15) family += (a >> 20) & 255;
        if (family == 6 || family >= 15) model |= ((a >> 16) & 15) << 4;
        tsc = (d >> 4) & 1;
        mmx = (d >> 23) & 1;
        sse = (d >> 25) & 1;
        sse2 = (d >> 26) & 1;
    }
    /* the name the CPU gives itself, and whether its counter is steady */
    char brand[49];
    brand[0] = 0;
    unsigned max_ext = 0;
    if (__get_cpuid(0x80000000u, &a, &b, &c, &d)) max_ext = a;
    if (max_ext >= 0x80000004u) {
        unsigned *w = (unsigned *)brand;
        for (unsigned k = 0; k < 3; ++k)
            if (__get_cpuid(0x80000002u + k, &a, &b, &c, &d)) {
                w[k * 4 + 0] = a;
                w[k * 4 + 1] = b;
                w[k * 4 + 2] = c;
                w[k * 4 + 3] = d;
            }
        brand[48] = 0;
    }
    if (max_ext >= 0x80000007u && __get_cpuid(0x80000007u, &a, &b, &c, &d)) sys->invariant_tsc = tsc && ((d >> 8) & 1);
    if (__get_cpuid_max(0, NULL) >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        avx2 = (b >> 5) & 1;
    }
    const char *name = brand;
    while (*name == ' ') ++name; /* (Intel pads it on the left) */
    if (tsc) {
        u64 t0 = plat_time_us(), c0 = read_tsc();
        while (plat_time_us() - t0 < 100000) { }
        u64 t1 = plat_time_us(), c1 = read_tsc();
        if (t1 > t0) {
            g_tsc_khz = (c1 - c0) * 1000u / (t1 - t0);
            sys->cpu_mhz = (u32)(g_tsc_khz / 1000u);
        }
    }
    /* (SSE needs the OS to save its registers: NT 4.0 and older never do,
     * so only MMX and plain x86 are used anywhere) */
    char model_txt[48];
    ub_fmt(model_txt, sizeof model_txt, "family %u model %u", family, model);
    ub_fmt(sys->cpu_desc, sizeof sys->cpu_desc, "%s%s%s%s%s%s%s%s%s, ~%u MHz%s", name[0] ? name : vendor,
           name[0] ? " (" : " ", model_txt, name[0] ? ")" : "", mmx ? ", MMX" : "", sse ? ", SSE" : "",
           sse2 ? ", SSE2" : "", avx2 ? ", AVX2" : "", "", sys->cpu_mhz, sys->invariant_tsc ? ", steady TSC" : "");
}

typedef struct {
    DWORD dwLength, dwMemoryLoad;
    DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile,
        ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} ub_memstatusex;
typedef BOOL(WINAPI *pfn_GlobalMemoryStatusEx)(ub_memstatusex *);

static void query_system(ub_system_info *sys)
{
    /* GetVersion (not GetVersionEx) keeps us on the NT 3.1-era API set.
     * Low byte = major, next byte = minor, high word = build on NT. */
    DWORD v = GetVersion();
    sys->os_major = v & 0xFF;
    sys->os_minor = (v >> 8) & 0xFF;
    sys->os_build = (v & 0x80000000u) ? 0 : (v >> 16) & 0x7FFF;
    const char *family = (v & 0x80000000u) ? "Windows (non-NT)" : "Windows NT";
    if (!(v & 0x80000000u) && sys->os_major == 5 && sys->os_minor == 0) family = "Windows 2000";
    if (!(v & 0x80000000u) && sys->os_major == 5 && sys->os_minor >= 1) family = "Windows XP/2003";
    if (!(v & 0x80000000u) && sys->os_major >= 6) family = "Windows";
    ub_fmt(sys->os_name, sizeof sys->os_name, "%s %u.%u build %u", family,
           sys->os_major, sys->os_minor, sys->os_build);

    /* RAM: GlobalMemoryStatusEx (Windows 2000+) reports >4 GB correctly;
     * fall back to GlobalMemoryStatus, which exists everywhere. */
    HMODULE k32 = GetModuleHandleA("KERNEL32.DLL");
    pfn_GlobalMemoryStatusEx gmsx =
        k32 ? (pfn_GlobalMemoryStatusEx)(void *)GetProcAddress(k32, "GlobalMemoryStatusEx") : NULL;
    ub_memstatusex mx;
    memset(&mx, 0, sizeof mx);
    mx.dwLength = sizeof mx;
    if (gmsx && gmsx(&mx)) {
        sys->total_phys_bytes = mx.ullTotalPhys;
    } else {
        MEMORYSTATUS ms;
        memset(&ms, 0, sizeof ms);
        ms.dwLength = sizeof ms;
        GlobalMemoryStatus(&ms);
        sys->total_phys_bytes = ms.dwTotalPhys;
    }

    HDC dc = GetDC(NULL);
    sys->display_bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    sys->palette_display = (GetDeviceCaps(dc, RASTERCAPS) & RC_PALETTE) != 0;
    ReleaseDC(NULL, dc);

    /* Unknown indexes return 0 on older systems, so this is safe on 3.51. */
    sys->has_mouse_wheel = GetSystemMetrics(SM_MOUSEWHEELPRESENT) != 0;
    query_cpu(sys);
    sys->cpu_count = plat_cpu_count();
}

/* ------------------------------------------------------------ processors
 *
 * NT 3.51 and 4.0 run on several processors (and do so in a virtual
 * machine given several). The fast renderer shares a frame out over them:
 * a few worker threads made on first use wait on an event each; the
 * calling thread takes its share of the jobs, then waits for theirs. */

#define MAX_WORKERS 15
static struct {
    HANDLE go, done;
} g_workers[MAX_WORKERS];
static int g_worker_count, g_cpu_count;
static void (*g_job_fn)(void *ctx, int job);
static void *g_job_ctx;
static int g_job_count, g_job_threads;

int plat_cpu_count(void)
{
    if (!g_cpu_count) {
        SYSTEM_INFO si;
        memset(&si, 0, sizeof si);
        GetSystemInfo(&si);
        g_cpu_count = si.dwNumberOfProcessors >= 1 ? (int)si.dwNumberOfProcessors : 1;
        if (g_cpu_count > 64) g_cpu_count = 64;
    }
    return g_cpu_count;
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    int k = (int)(INT_PTR)arg;
    for (;;) {
        WaitForSingleObject(g_workers[k].go, INFINITE);
        for (int j = k + 1; j < g_job_count; j += g_job_threads) g_job_fn(g_job_ctx, j);
        SetEvent(g_workers[k].done);
    }
    return 0; /* (never) */
}

void plat_parallel(void (*fn)(void *ctx, int job), void *ctx, int count)
{
    int want = count < plat_cpu_count() ? count : plat_cpu_count();
    if (want - 1 > MAX_WORKERS) want = MAX_WORKERS + 1;
    while (g_worker_count < want - 1) {
        int k = g_worker_count;
        g_workers[k].go = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_workers[k].done = CreateEventW(NULL, FALSE, FALSE, NULL);
        DWORD id;
        HANDLE t = g_workers[k].go && g_workers[k].done
                       ? CreateThread(NULL, 64 * 1024, worker_main, (LPVOID)(INT_PTR)k, 0, &id)
                       : NULL;
        if (!t) {
            if (g_workers[k].go) CloseHandle(g_workers[k].go);
            if (g_workers[k].done) CloseHandle(g_workers[k].done);
            ub_warn("could not start a drawing thread; using %d", g_worker_count + 1);
            break;
        }
        CloseHandle(t); /* it runs until the process ends */
        g_worker_count++;
    }
    int helpers = want - 1 < g_worker_count ? want - 1 : g_worker_count;
    if (helpers < 0) helpers = 0;
    g_job_fn = fn;
    g_job_ctx = ctx;
    g_job_count = count;
    g_job_threads = helpers + 1;
    for (int k = 0; k < helpers; ++k) SetEvent(g_workers[k].go);
    for (int j = 0; j < count; j += helpers + 1) fn(ctx, j);
    for (int k = 0; k < helpers; ++k) WaitForSingleObject(g_workers[k].done, INFINITE);
}

/* ---------------------------------------------------------------- input */

static bool any_mouse_down(void)
{
    for (int i = 0; i < UB_MOUSE_BUTTONS; ++i) {
        if (g_input.mouse_down[i]) return true;
    }
    return false;
}

static void client_center_screen(POINT *c, RECT *clip)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    c->x = (tl.x + br.x) / 2;
    c->y = (tl.y + br.y) / 2;
    if (clip) {
        clip->left = tl.x;
        clip->top = tl.y;
        clip->right = br.x;
        clip->bottom = br.y;
    }
}

void plat_set_mouse_capture(bool on)
{
    if (on && !g_input.has_focus) return;
    if (on == g_look) return;
    g_look = on;
    g_input.mouse_captured = on;
    if (on) {
        POINT c;
        RECT clip;
        GetCursorPos(&g_look_restore);
        SetCapture(g_hwnd);
        ShowCursor(FALSE);
        client_center_screen(&c, &clip);
        ClipCursor(&clip);
        SetCursorPos(c.x, c.y);
    } else {
        ClipCursor(NULL);
        SetCursorPos(g_look_restore.x, g_look_restore.y);
        ShowCursor(TRUE);
        if (!any_mouse_down()) ReleaseCapture();
    }
}

void plat_request_quit(void)
{
    g_input.quit_requested = true;
}

static void begin_input_frame(void)
{
    memset(g_input.key_pressed, 0, sizeof g_input.key_pressed);
    memset(g_input.key_released, 0, sizeof g_input.key_released);
    memset(g_input.key_repeat, 0, sizeof g_input.key_repeat);
    memset(g_input.mouse_pressed, 0, sizeof g_input.mouse_pressed);
    memset(g_input.mouse_released, 0, sizeof g_input.mouse_released);
    g_input.mouse_dx = g_input.mouse_dy = 0;
    g_input.wheel = 0;
    g_input.text[0] = 0;
    g_text_len = 0;
}

static void poll_mouse_look(void)
{
    if (!g_look) return;
    POINT p, c;
    RECT clip;
    GetCursorPos(&p);
    client_center_screen(&c, &clip);
    g_input.mouse_dx += p.x - c.x;
    g_input.mouse_dy += p.y - c.y;
    ClipCursor(&clip); /* the window may have moved */
    SetCursorPos(c.x, c.y);
}

static void release_everything(void)
{
    for (int k = 0; k < UB_KEY_COUNT; ++k) {
        if (g_input.key_down[k]) g_input.key_released[k] = true;
        g_input.key_down[k] = false;
    }
    for (int b = 0; b < UB_MOUSE_BUTTONS; ++b) {
        if (g_input.mouse_down[b]) g_input.mouse_released[b] = true;
        g_input.mouse_down[b] = false;
    }
    plat_set_mouse_capture(false);
    ReleaseCapture();
}

static void add_text_char(u32 cp)
{
    char tmp[4];
    int n = ub_utf8_encode(cp, tmp);
    if (g_text_len + n >= UB_TEXT_INPUT_MAX) return;
    for (int i = 0; i < n; ++i) g_input.text[g_text_len++] = tmp[i];
    g_input.text[g_text_len] = 0;
}

static void on_char(WPARAM wp)
{
    u32 c = (u32)(wp & 0xFFFF);
    if (c >= 0xD800 && c <= 0xDBFF) {
        g_high_surrogate = (u16)c;
        return;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (!g_high_surrogate) return;
        c = 0x10000 + (((u32)g_high_surrogate - 0xD800) << 10) + (c - 0xDC00);
    }
    g_high_surrogate = 0;
    /* keep the editing keys in the stream so their order is preserved */
    if ((c < 32 && c != '\b' && c != '\r' && c != 0x1B) || c == 127) return;
    add_text_char(c);
}

static void on_mouse_button(int button, bool down, LPARAM lp)
{
    if (down && !g_input.mouse_down[button]) g_input.mouse_pressed[button] = true;
    if (!down && g_input.mouse_down[button]) g_input.mouse_released[button] = true;
    g_input.mouse_down[button] = down;
    if (down) {
        SetCapture(g_hwnd); /* keep getting the button-up outside the window */
    } else if (!any_mouse_down() && !g_look) {
        ReleaseCapture();
    }
    (void)lp;
}

static void update_mouse_pos(LPARAM lp)
{
    if (g_look) return; /* the pointer is pinned to the centre */
    g_input.mouse_x = (short)LOWORD(lp);
    g_input.mouse_y = (short)HIWORD(lp);
}

/* ------------------------------------------------------------ window */

void plat_set_title(const char *title)
{
    u16 wide[256];
    ub_utf8_to_utf16(title, wide, 256);
    if (g_hwnd) SetWindowTextW(g_hwnd, (LPCWSTR)wide);
}

void plat_set_fps_cap(u32 fps)
{
    g_frame.fps_cap = fps > 1000 ? 1000 : fps;
    g_next_frame_us = 0;
    ub_info("display: frame rate cap %u%s", g_frame.fps_cap, g_frame.fps_cap ? " fps" : " (unlimited)");
}

typedef HMONITOR(WINAPI *pfn_MonitorFromWindow)(HWND, DWORD);
typedef BOOL(WINAPI *pfn_GetMonitorInfoW)(HMONITOR, MONITORINFO *);

/* The monitor the window is on (Windows 98/2000+), or the primary screen. */
static RECT monitor_rect(HWND hwnd)
{
    RECT r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMODULE user = GetModuleHandleA("USER32.DLL");
    pfn_MonitorFromWindow from_window =
        user ? (pfn_MonitorFromWindow)(void *)GetProcAddress(user, "MonitorFromWindow") : NULL;
    pfn_GetMonitorInfoW get_info =
        user ? (pfn_GetMonitorInfoW)(void *)GetProcAddress(user, "GetMonitorInfoW") : NULL;
    if (from_window && get_info) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (get_info(from_window(hwnd, 2 /* MONITOR_DEFAULTTONEAREST */), &mi)) r = mi.rcMonitor;
    }
    return r;
}

/* ------------------------------------------------ fullscreen display modes
 * ChangeDisplaySettingsW / EnumDisplaySettingsW arrived with NT 4.0: they
 * are looked up at run time, so NT 3.51 simply has no modes to offer. */
typedef LONG(WINAPI *pfn_ChangeDisplaySettingsW)(DEVMODEW *, DWORD);
typedef BOOL(WINAPI *pfn_EnumDisplaySettingsW)(LPCWSTR, DWORD, DEVMODEW *);
static pfn_ChangeDisplaySettingsW p_cds;
static pfn_EnumDisplaySettingsW p_eds;
static bool g_mode_api_tried;
static int g_fs_w, g_fs_h;           /* wanted fullscreen mode, 0 = the desktop's */
static bool g_mode_changed;          /* the screen is in our mode now */
static bool g_mode_suspended;        /* switched away: desktop mode back until we return */

static void load_mode_api(void)
{
    if (g_mode_api_tried) return;
    g_mode_api_tried = true;
    HMODULE user = GetModuleHandleA("USER32.DLL");
    if (!user) return;
    p_cds = (pfn_ChangeDisplaySettingsW)(void *)GetProcAddress(user, "ChangeDisplaySettingsW");
    p_eds = (pfn_EnumDisplaySettingsW)(void *)GetProcAddress(user, "EnumDisplaySettingsW");
    if (!p_cds || !p_eds) p_cds = NULL;
}

static int desktop_bpp(void)
{
    HDC dc = GetDC(NULL);
    int bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    ReleaseDC(NULL, dc);
    return bpp;
}

int plat_display_modes(ub_display_mode *out, int max)
{
    load_mode_api();
    if (!p_cds) return 0;
    int bpp = desktop_bpp(), n = 0;
    DEVMODEW dm;
    for (DWORD i = 0; i < 400 && n < max; ++i) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!p_eds(NULL, i, &dm)) break;
        if ((int)dm.dmBitsPerPel != bpp || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        bool seen = false;
        for (int k = 0; k < n && !seen; ++k) seen = out[k].w == (int)dm.dmPelsWidth && out[k].h == (int)dm.dmPelsHeight;
        if (seen) continue;
        int at = n++; /* keep them smallest first */
        while (at > 0 && (i64)out[at - 1].w * out[at - 1].h > (i64)dm.dmPelsWidth * dm.dmPelsHeight) {
            out[at] = out[at - 1];
            --at;
        }
        out[at].w = (int)dm.dmPelsWidth;
        out[at].h = (int)dm.dmPelsHeight;
    }
    return n;
}

/* Switches the screen to the wanted mode (ours) or back to the desktop's. */
static bool screen_mode(bool ours)
{
    if (!ours) {
        if (g_mode_changed) {
            p_cds(NULL, 0);
            g_mode_changed = false;
            ub_info("display: the desktop's own mode is back");
        }
        return true;
    }
    load_mode_api();
    if (!p_cds || g_fs_w <= 0 || g_fs_h <= 0) return false;
    int bpp = desktop_bpp();
    DEVMODEW dm, best;
    bool found = false;
    for (DWORD i = 0; i < 400; ++i) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!p_eds(NULL, i, &dm)) break;
        if ((int)dm.dmPelsWidth != g_fs_w || (int)dm.dmPelsHeight != g_fs_h || (int)dm.dmBitsPerPel != bpp) continue;
        /* the highest refresh rate up to 120 Hz */
        if (!found || (dm.dmDisplayFrequency > best.dmDisplayFrequency && dm.dmDisplayFrequency <= 120)) best = dm;
        found = true;
    }
    if (!found) {
        ub_warn("display: no %dx%d mode at %d bits; staying at the desktop's", g_fs_w, g_fs_h, bpp);
        return false;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | (best.dmDisplayFrequency ? DM_DISPLAYFREQUENCY : 0);
    LONG r = p_cds(&best, CDS_FULLSCREEN);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        ub_warn("display: the screen would not switch to %dx%d (code %d); staying at the desktop's", g_fs_w, g_fs_h, (int)r);
        return false;
    }
    g_mode_changed = true;
    ub_info("display: switched the screen to %dx%d, %d bits, %u Hz", g_fs_w, g_fs_h, bpp, (u32)best.dmDisplayFrequency);
    return true;
}

static void cover_screen(void)
{
    RECT m = monitor_rect(g_hwnd);
    SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void plat_set_fullscreen_mode(int w, int h)
{
    if (w == g_fs_w && h == g_fs_h) return;
    g_fs_w = w;
    g_fs_h = h;
    if (!g_hwnd || !g_frame.fullscreen) return;
    screen_mode(false);
    screen_mode(true);
    cover_screen();
    win32_sw_invalidate();
}

static void apply_fullscreen(bool on)
{
    if (!g_hwnd || on == g_frame.fullscreen) return;
    plat_set_mouse_capture(false);
    if (on) {
        GetWindowRect(g_hwnd, &g_windowed_rect);
        screen_mode(true);
        RECT m = monitor_rect(g_hwnd);
        SetWindowLongW(g_hwnd, GWL_STYLE, FULLSCREEN_STYLE);
        SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        screen_mode(false);
        g_mode_suspended = false;
        SetWindowLongW(g_hwnd, GWL_STYLE, WINDOWED_STYLE);
        RECT r = g_windowed_rect;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_frame.fullscreen = on;
    ub_info("display: %s", on ? "fullscreen" : "windowed");
}

void plat_set_fullscreen(bool on)
{
    g_want_fullscreen = on; /* applied between frames */
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT pal_result;
    if (g_frame.backend == UB_BACKEND_SOFTWARE && win32_sw_palette_message(hwnd, msg, wp, &pal_result))
        return pal_result;
    if (g_msh_wheel_msg && msg == g_msh_wheel_msg) {
        g_input.wheel += (int)wp;
        return 0;
    }
    switch (msg) {
    case WM_CLOSE:
        g_input.quit_requested = true;
        return 0;
    case WM_DESTROY:
        return 0;
    case WM_SETFOCUS:
        g_input.has_focus = true;
        return 0;
    case WM_ACTIVATEAPP:
        /* in our own display mode: give the desktop its mode back while
         * another program is in front, and take ours again on return */
        if (!wp && g_mode_changed) {
            screen_mode(false);
            g_mode_suspended = true;
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (wp && g_mode_suspended && g_frame.fullscreen) {
            g_mode_suspended = false;
            ShowWindow(hwnd, SW_RESTORE);
            if (screen_mode(true)) cover_screen();
            win32_sw_invalidate();
        }
        break;
    case WM_KILLFOCUS:
        g_input.has_focus = false;
        release_everything();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int k = (int)(wp & 0xFF);
        bool was_down = (lp & (1L << 30)) != 0; /* auto-repeat */
        if (!was_down) g_input.key_pressed[k] = true;
        g_input.key_repeat[k] = true;
        g_input.key_down[k] = true;
        /* Let Alt+F4 through; swallow other system keys so a lone Alt or
         * F10 does not freeze the game loop in menu mode. */
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int k = (int)(wp & 0xFF);
        if (g_input.key_down[k]) g_input.key_released[k] = true;
        g_input.key_down[k] = false;
        return 0;
    }
    case WM_CHAR:
        on_char(wp);
        return 0;
    case WM_SYSCHAR:
        return 0; /* no beep for Alt+letter */
    case WM_MOUSEMOVE:
        update_mouse_pos(lp);
        return 0;
    case WM_LBUTTONDOWN: update_mouse_pos(lp); on_mouse_button(UB_MOUSE_LEFT, true, lp); return 0;
    case WM_LBUTTONUP:   update_mouse_pos(lp); on_mouse_button(UB_MOUSE_LEFT, false, lp); return 0;
    case WM_RBUTTONDOWN: update_mouse_pos(lp); on_mouse_button(UB_MOUSE_RIGHT, true, lp); return 0;
    case WM_RBUTTONUP:   update_mouse_pos(lp); on_mouse_button(UB_MOUSE_RIGHT, false, lp); return 0;
    case WM_MBUTTONDOWN: update_mouse_pos(lp); on_mouse_button(UB_MOUSE_MIDDLE, true, lp); return 0;
    case WM_MBUTTONUP:   update_mouse_pos(lp); on_mouse_button(UB_MOUSE_MIDDLE, false, lp); return 0;
    case WM_MOUSEWHEEL:
        g_input.wheel += (short)HIWORD(wp);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* we paint every pixel */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_frame.backend == UB_BACKEND_SOFTWARE && g_frame.fb) {
            win32_sw_invalidate();
            win32_sw_present(dc, &g_frame);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool register_class(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    /* CS_OWNDC: OpenGL needs a DC that lives as long as the window */
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1)); /* from res/ */
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"UnibloxWindow";
    return RegisterClassW(&wc) != 0;
}

/* Window size: from the prefs, or the largest 16:9 area that fits in
 * three quarters of the screen (but at least 640x480). */
static void initial_size(const ub_display_prefs *prefs, const ub_system_info *sys, int *w, int *h)
{
    if (prefs->window_w >= 320 && prefs->window_h >= 240) {
        *w = prefs->window_w;
        *h = prefs->window_h;
    } else {
        int sw = sys->screen_w > 0 ? sys->screen_w : 800, sh = sys->screen_h > 0 ? sys->screen_h : 600;
        *w = sw * 3 / 4;
        *h = *w * 9 / 16;
        if (*h > sh * 3 / 4) {
            *h = sh * 3 / 4;
            *w = *h * 16 / 9;
        }
        if (*w < UB_DEFAULT_WINDOW_W) *w = UB_DEFAULT_WINDOW_W;
        if (*h < UB_DEFAULT_WINDOW_H) *h = UB_DEFAULT_WINDOW_H;
    }
    if (sys->screen_w > 0 && *w > sys->screen_w) *w = sys->screen_w;
    if (sys->screen_h > 0 && *h > sys->screen_h) *h = sys->screen_h;
}

static bool create_window(int client_w, int client_h)
{
    RECT r = { 0, 0, client_w, client_h };
    AdjustWindowRect(&r, WINDOWED_STYLE, FALSE);
    u16 title[96];
    ub_utf8_to_utf16(app_info()->title, title, 96);
    g_hwnd = CreateWindowW(L"UnibloxWindow", (LPCWSTR)title, WINDOWED_STYLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, g_inst, NULL);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    g_input.has_focus = GetFocus() == g_hwnd;
    g_frame.fullscreen = false;
    return true;
}

/* OpenGL if wanted and usable; otherwise software. A window's pixel
 * format can be set only once, so a failed OpenGL attempt gets a fresh
 * window for the software renderer. */
static void choose_backend(const ub_display_prefs *prefs, ub_system_info *sys, int w, int h)
{
    g_frame.backend = UB_BACKEND_SOFTWARE;
    if (prefs->backend == UB_BACKEND_PREF_SOFTWARE || prefs->backend == UB_BACKEND_PREF_FAST) {
        ub_info("display: %s renderer chosen in settings", prefs->backend == UB_BACKEND_PREF_FAST ? "fast software" : "software");
        return;
    }
    if (sys->display_bpp <= 8) {
        ub_info("display: 256-colour desktop; using the software renderer");
        return;
    }
    bool generic = false;
    if (win32_gl_init(g_hwnd, prefs->vsync, &generic)) {
        if (generic && prefs->backend == UB_BACKEND_PREF_AUTO) {
            ub_info("display: the only OpenGL here renders in software; Uniblox's own "
                    "fast software renderer is faster (set renderer=opengl to use it anyway)");
            win32_gl_shutdown();
        } else {
            g_frame.backend = UB_BACKEND_OPENGL;
            return;
        }
    }
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    create_window(w, h);
}

/* ----------------------------------------------------------------- main */

static int fatal(const WCHAR *what)
{
    char utf8[512];
    ub_utf16_to_utf8((const u16 *)what, utf8, sizeof utf8); /* WCHAR is 16-bit on Win32 */
    ub_error("fatal: %s", utf8);
    ub_log_close();
    u16 title[96];
    ub_utf8_to_utf16(app_info()->title, title, 96);
    MessageBoxW(NULL, what, (LPCWSTR)title, MB_OK | MB_ICONSTOP);
    return 1;
}

typedef BOOL(WINAPI *pfn_SetProcessDPIAware)(void);

/* Wait until the next frame is due. Sleep() covers most of the wait; the
 * last ~1.5 ms is spun on Sleep(0) because Sleep's 1 ms granularity alone
 * cannot hold 120 fps (8.33 ms) steady. */
static void pace_frame(void)
{
    if (!g_frame.fps_cap) {
        g_frame.sleep_us = 0;
        return;
    }
    u64 period = 1000000u / g_frame.fps_cap;
    u64 now = plat_time_us(), start = now;
    if (!g_next_frame_us) g_next_frame_us = now;
    g_next_frame_us += period;
    if (g_next_frame_us <= now) {
        /* running slower than the cap: never wait; if a whole period
         * behind, restart the schedule rather than racing to catch up */
        if (now - g_next_frame_us > period) g_next_frame_us = now;
        g_frame.sleep_us = 0;
        return;
    }
    while ((now = plat_time_us()) < g_next_frame_us) {
        u64 left = g_next_frame_us - now;
        Sleep(left > 2000 ? (DWORD)((left - 1500) / 1000u) : 0);
    }
    g_frame.sleep_us = (u32)(now - start);
}

static int win32_main(void)
{
    g_inst = GetModuleHandleW(NULL);
    /* Real pixels on high-DPI monitors (Vista+); without this Windows
     * renders us small and upscales the result, which looks blurry. */
    HMODULE user = GetModuleHandleA("USER32.DLL");
    pfn_SetProcessDPIAware dpi_aware =
        user ? (pfn_SetProcessDPIAware)(void *)GetProcAddress(user, "SetProcessDPIAware") : NULL;
    if (dpi_aware) dpi_aware();

    timeBeginPeriod(1); /* 1 ms Sleep() granularity for the frame limiter */
    win32_init_base_dir();
    ub_log_open(app_info()->log_path, app_info()->old_log_path);

    ub_system_info sys;
    memset(&sys, 0, sizeof sys);
    query_system(&sys);
    sys.screen_w = GetSystemMetrics(SM_CXSCREEN);
    sys.screen_h = GetSystemMetrics(SM_CYSCREEN);
    {
        char base[MAX_PATH * 3];
        ub_utf16_to_utf8((const u16 *)win32_base_dir(), base, sizeof base);
        ub_info("running from %s", base[0] ? base : "(current directory)");
    }
    ub_info("%s, %u MB RAM, %dx%d %d bpp screen%s, mouse wheel %s", sys.os_name,
            (u32)(sys.total_phys_bytes >> 20), sys.screen_w, sys.screen_h, sys.display_bpp,
            dpi_aware ? " (DPI-aware)" : "", sys.has_mouse_wheel ? "reported" : "not reported");
    ub_info("CPU: %s; %d processor%s", sys.cpu_desc, sys.cpu_count, sys.cpu_count == 1 ? "" : "s");
    g_msh_wheel_msg = RegisterWindowMessageW(L"MSWHEEL_ROLLMSG");

    ub_display_prefs prefs;
    memset(&prefs, 0, sizeof prefs);
    prefs.fps_cap = 120;
    prefs.sw_height = 480;
    app_preinit(&prefs);
    if (sys.invariant_tsc && g_tsc_khz > 1000 && !prefs.timer_qpc) {
        win32_use_tsc(g_tsc_khz);
        ub_info("timer: the CPU's steady time-stamp counter (%u kHz)", (u32)g_tsc_khz);
    } else {
        ub_info("timer: QueryPerformanceCounter%s", sys.invariant_tsc ? " (timer=qpc in the settings)" : "");
    }

    int w, h;
    initial_size(&prefs, &sys, &w, &h);
    if (!register_class() || !create_window(w, h)) return fatal(L"Could not create the main window.");
    choose_backend(&prefs, &sys, w, h);
    if (!g_hwnd) return fatal(L"Could not create the main window.");
    if (g_frame.backend == UB_BACKEND_SOFTWARE) win32_sw_init(&sys, prefs.sw_height);
    g_frame.fps_cap = prefs.fps_cap;
    ub_info("display: %s renderer, %dx%d window, frame rate cap %u%s",
            g_frame.backend == UB_BACKEND_OPENGL ? "OpenGL" : "software", w, h, g_frame.fps_cap,
            g_frame.fps_cap ? " fps" : " (unlimited)");
    g_fs_w = prefs.fs_width;
    g_fs_h = prefs.fs_height;
    if (prefs.fullscreen) apply_fullscreen(true);
    g_want_fullscreen = g_frame.fullscreen;

    win32_audio_open(&sys);
    {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_frame.width = rc.right;
        g_frame.height = rc.bottom;
        if (g_frame.backend == UB_BACKEND_SOFTWARE) g_frame.fb = win32_sw_prepare(rc.right, rc.bottom);
    }
    if (!app_init(&sys, &g_frame)) return fatal(L"Uniblox failed to initialise. See the log file next to the program.");

    u64 last = plat_time_us();
    MSG msg;
    while (!g_input.quit_requested) {
        begin_input_frame();
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_input.quit_requested = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        poll_mouse_look();
        if (g_want_fullscreen != g_frame.fullscreen) apply_fullscreen(g_want_fullscreen);

        RECT rc;
        GetClientRect(g_hwnd, &rc);
        u64 now = plat_time_us();
        u32 dt = (u32)(now - last);
        last = now;
        if (rc.right <= 0 || rc.bottom <= 0) { /* minimised: idle */
            Sleep(20);
            continue;
        }
        g_frame.width = rc.right;
        g_frame.height = rc.bottom;
        if (g_frame.backend == UB_BACKEND_SOFTWARE) {
            g_frame.fb = win32_sw_prepare(rc.right, rc.bottom);
            if (!g_frame.fb) return fatal(L"Out of memory for the frame buffer.");
        }

        app_frame(&g_frame, &g_input, dt);
        if (g_input.quit_requested) break;

        u64 t_present = plat_time_us();
        if (g_frame.backend == UB_BACKEND_OPENGL) {
            win32_gl_swap();
        } else {
            HDC dc = GetDC(g_hwnd);
            win32_sw_present(dc, &g_frame);
            ReleaseDC(g_hwnd, dc);
        }
        g_frame.present_us = (u32)(plat_time_us() - t_present);
        pace_frame();
    }

    ub_info("shutting down");
    plat_set_mouse_capture(false);
    screen_mode(false);
    app_shutdown();
    win32_audio_close();
    ub_mixer_shutdown();
    if (g_frame.backend == UB_BACKEND_OPENGL) win32_gl_shutdown();
    else win32_sw_shutdown();
    timeEndPeriod(1);
    DestroyWindow(g_hwnd);
    ub_info("shutdown complete");
    ub_log_close();
    return 0;
}

/* Process entry point (set with -e in the Makefile). There is no CRT, so
 * nothing runs before this: no static constructors, no argv parsing. */
void __cdecl ub_entry(void)
{
    ExitProcess((UINT)win32_main());
}
