/* Uniblox - Win32 window, input and main loop.
 *
 * Display path: the app renders top-down into either a 32-bit XRGB or an
 * 8-bit palette-index buffer. We copy that into a bottom-up DIB (24-bit,
 * or 8-bit with a colour table; formats every Windows version supports),
 * stretch it into an off-screen bitmap, draw overlay text with GDI, then
 * BitBlt to the window so there is no flicker.
 *
 * On 256-colour desktops we build a Windows palette from the app's colours
 * (keeping the 20 static system colours), realise it, and blit with
 * DIB_PAL_COLORS so pixels map straight to palette slots with no colour
 * matching per frame.
 *
 * Input: keyboard via WM_KEYDOWN/WM_KEYUP/WM_CHAR, mouse via the button
 * and WM_MOUSEMOVE messages.
 * Mouse-look re-centres the cursor every frame and reports the offset, the
 * only relative-mouse method that works on NT 3.51 (Raw Input is XP+).
 * The wheel arrives as WM_MOUSEWHEEL (NT 4.0+) or the registered
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

static HWND g_hwnd;
static ub_input g_input;
static ub_overlay g_overlay;
static ub_framebuffer g_fb;
static u8 *g_dib;            /* 24bpp bottom-up copy of g_fb (XRGB32 format) */
static int g_dib_pitch;      /* bytes, DWORD aligned */
static BITMAPINFO g_bmi;
static u8 *g_dib8;           /* 8bpp bottom-up copy of g_fb (INDEX8 format) */
static int g_dib8_pitch;
static struct { BITMAPINFOHEADER h; RGBQUAD colors[256]; } g_bmi8rgb;
static struct { BITMAPINFOHEADER h; WORD index[256]; } g_bmi8pal;
static bool g_palette_device;   /* desktop is 256 colours with a hardware palette */
static HPALETTE g_hpal;
static u32 g_palette_serial_seen = 0xFFFFFFFFu;
static ub_pixel_format g_pending_format;
static HDC g_memdc;
static HBITMAP g_membmp, g_oldbmp;
static int g_mem_w, g_mem_h;
static UINT g_msh_wheel_msg; /* registered wheel message, 0 if unavailable */
static int g_text_len;
static u16 g_high_surrogate;
static bool g_look;          /* mouse-look active */
static POINT g_look_restore; /* cursor position to restore afterwards */

/* ---------------------------------------------------------- system info */

static u64 read_tsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

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
    bool mmx = false, sse = false, tsc = false;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        family = (a >> 8) & 15;
        model = (a >> 4) & 15;
        if (family == 15) family += (a >> 20) & 255;
        if (family == 6 || family >= 15) model |= ((a >> 16) & 15) << 4;
        tsc = (d >> 4) & 1;
        mmx = (d >> 23) & 1;
        sse = (d >> 25) & 1;
    }
    if (tsc) {
        u64 t0 = plat_time_us(), c0 = read_tsc();
        while (plat_time_us() - t0 < 50000) { }
        u64 t1 = plat_time_us(), c1 = read_tsc();
        if (t1 > t0) sys->cpu_mhz = (u32)((c1 - c0) / (t1 - t0));
    }
    ub_fmt(sys->cpu_desc, sizeof sys->cpu_desc, "%s family %u model %u%s%s, ~%u MHz",
           vendor, family, model, mmx ? ", MMX" : "", sse ? ", SSE" : "", sys->cpu_mhz);
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
}

/* ------------------------------------------------------------- display */

void plat_set_pixel_format(ub_pixel_format f)
{
    g_pending_format = f;
}

static bool create_framebuffer(const ub_system_info *sys)
{
    g_fb.width = UB_FB_WIDTH;
    g_fb.height = UB_FB_HEIGHT;
    g_fb.pitch = UB_FB_WIDTH;
    g_fb.pixels = (u32 *)plat_alloc((size_t)g_fb.pitch * g_fb.height * 4);
    g_fb.pixels8 = (u8 *)plat_alloc((size_t)g_fb.pitch * g_fb.height);
    g_dib_pitch = (g_fb.width * 3 + 3) & ~3;
    g_dib = (u8 *)plat_alloc((size_t)g_dib_pitch * g_fb.height);
    g_dib8_pitch = (g_fb.width + 3) & ~3;
    g_dib8 = (u8 *)plat_alloc((size_t)g_dib8_pitch * g_fb.height);
    if (!g_fb.pixels || !g_fb.pixels8 || !g_dib || !g_dib8) return false;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = g_fb.width;
    g_bmi.bmiHeader.biHeight = g_fb.height; /* positive: bottom-up */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 24;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_bmi8rgb.h = g_bmi.bmiHeader;
    g_bmi8rgb.h.biBitCount = 8;
    g_bmi8rgb.h.biClrUsed = 256;
    g_bmi8pal.h = g_bmi8rgb.h;
    for (int i = 0; i < 256; ++i) g_bmi8pal.index[i] = (WORD)i; /* identity */

    g_palette_device = sys->palette_display;
    g_fb.format = sys->display_bpp <= 8 ? UB_PIXEL_INDEX8 : UB_PIXEL_XRGB32;
    g_pending_format = g_fb.format;
    return true;
}

/* Copy the app's top-down frame into the bottom-up DIB for its format. */
static void convert_framebuffer(void)
{
    if (g_fb.format == UB_PIXEL_INDEX8) {
        for (int y = 0; y < g_fb.height; ++y)
            memcpy(g_dib8 + (size_t)(g_fb.height - 1 - y) * g_dib8_pitch,
                   g_fb.pixels8 + y * g_fb.pitch, (size_t)g_fb.width);
        return;
    }
    for (int y = 0; y < g_fb.height; ++y) {
        const u32 *src = g_fb.pixels + y * g_fb.pitch;
        u8 *dst = g_dib + (size_t)(g_fb.height - 1 - y) * g_dib_pitch;
        for (int x = 0; x < g_fb.width; ++x) {
            u32 c = src[x];
            dst[0] = (u8)c;         /* B */
            dst[1] = (u8)(c >> 8);  /* G */
            dst[2] = (u8)(c >> 16); /* R */
            dst += 3;
        }
    }
}

/* Rebuild the DIB colour table and, on palette displays, the Windows
 * palette whenever the app's palette changes (normally once). */
static void update_palette(HDC wdc)
{
    if (!g_fb.palette || g_fb.palette_serial == g_palette_serial_seen) return;
    g_palette_serial_seen = g_fb.palette_serial;

    PALETTEENTRY sys_colors[256];
    memset(sys_colors, 0, sizeof sys_colors);
    if (g_palette_device) GetSystemPaletteEntries(wdc, 0, 256, sys_colors);

    struct { WORD version, count; PALETTEENTRY e[256]; } lp;
    lp.version = 0x300;
    lp.count = 256;
    for (int i = 0; i < 256; ++i) {
        u32 c = g_fb.palette[i];
        bool reserved = i < 10 || i >= 246; /* Windows' 20 static colours */
        PALETTEENTRY e;
        if (reserved && g_palette_device) {
            e = sys_colors[i];
            e.peFlags = 0;
        } else {
            e.peRed = (BYTE)(c >> 16);
            e.peGreen = (BYTE)(c >> 8);
            e.peBlue = (BYTE)c;
            e.peFlags = reserved ? 0 : PC_NOCOLLAPSE;
        }
        lp.e[i] = e;
        g_bmi8rgb.colors[i].rgbRed = e.peRed;
        g_bmi8rgb.colors[i].rgbGreen = e.peGreen;
        g_bmi8rgb.colors[i].rgbBlue = e.peBlue;
        g_bmi8rgb.colors[i].rgbReserved = 0;
    }
    if (!g_palette_device) return;

    HPALETTE np = CreatePalette((LOGPALETTE *)&lp);
    if (!np) {
        ub_warn("CreatePalette failed; 256-colour output will use GDI colour matching");
        return;
    }
    /* select the new palette everywhere before deleting the old one */
    SelectPalette(wdc, np, FALSE);
    if (g_memdc) SelectPalette(g_memdc, np, FALSE);
    if (g_hpal) DeleteObject(g_hpal);
    g_hpal = np;
    ub_info("display: created 256-colour palette");
}

static void ensure_backbuffer(HDC wdc, int w, int h)
{
    if (g_memdc && w == g_mem_w && h == g_mem_h) return;
    if (g_memdc) {
        SelectObject(g_memdc, g_oldbmp);
        DeleteObject(g_membmp);
        DeleteDC(g_memdc);
    }
    g_memdc = CreateCompatibleDC(wdc);
    g_membmp = CreateCompatibleBitmap(wdc, w, h);
    g_oldbmp = (HBITMAP)SelectObject(g_memdc, g_membmp);
    g_mem_w = w;
    g_mem_h = h;
}

static void present(HDC wdc)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    ensure_backbuffer(wdc, w, h);
    update_palette(wdc);
    if (g_hpal) {
        SelectPalette(wdc, g_hpal, FALSE);
        RealizePalette(wdc);
        SelectPalette(g_memdc, g_hpal, FALSE);
        RealizePalette(g_memdc);
    }
    SetStretchBltMode(g_memdc, COLORONCOLOR);
    if (g_fb.format == UB_PIXEL_INDEX8) {
        if (g_hpal)
            StretchDIBits(g_memdc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                          g_dib8, (BITMAPINFO *)&g_bmi8pal, DIB_PAL_COLORS, SRCCOPY);
        else
            StretchDIBits(g_memdc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                          g_dib8, (BITMAPINFO *)&g_bmi8rgb, DIB_RGB_COLORS, SRCCOPY);
    } else {
        StretchDIBits(g_memdc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                      g_dib, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    SetBkMode(g_memdc, TRANSPARENT);
    for (int i = 0; i < g_overlay.count; ++i) {
        u16 wide[UB_OVERLAY_LINE_LEN];
        int n = ub_utf8_to_utf16(g_overlay.lines[i], wide, UB_OVERLAY_LINE_LEN);
        int y = 4 + i * 16;
        SetTextColor(g_memdc, RGB(0, 0, 0));
        TextOutW(g_memdc, 7, y + 1, (LPCWSTR)wide, n);   /* drop shadow */
        SetTextColor(g_memdc, RGB(255, 255, 255));
        TextOutW(g_memdc, 6, y, (LPCWSTR)wide, n);
    }

    BitBlt(wdc, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
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
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    if (rc.right > 0 && rc.bottom > 0) {
        g_input.mouse_x = mx * g_fb.width / rc.right;
        g_input.mouse_y = my * g_fb.height / rc.bottom;
    }
}

/* --------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_msh_wheel_msg && msg == g_msh_wheel_msg) {
        g_input.wheel += (int)wp;
        return 0;
    }
    switch (msg) {
    case WM_CLOSE:
        g_input.quit_requested = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SETFOCUS:
        g_input.has_focus = true;
        return 0;
    case WM_KILLFOCUS:
        g_input.has_focus = false;
        release_everything();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int k = (int)(wp & 0xFF);
        bool was_down = (lp & (1L << 30)) != 0; /* auto-repeat */
        if (!was_down) g_input.key_pressed[k] = true;
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
    case WM_QUERYNEWPALETTE: /* we are becoming active: claim the palette */
        if (g_hpal) {
            HDC dc = GetDC(hwnd);
            SelectPalette(dc, g_hpal, FALSE);
            UINT changed = RealizePalette(dc);
            ReleaseDC(hwnd, dc);
            if (changed) InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;
        }
        break;
    case WM_PALETTECHANGED: /* another window changed the system palette */
        if (g_hpal && (HWND)wp != hwnd) {
            HDC dc = GetDC(hwnd);
            SelectPalette(dc, g_hpal, FALSE);
            RealizePalette(dc);
            ReleaseDC(hwnd, dc);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1; /* we paint every pixel */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_fb.pixels) present(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#define WINDOW_STYLE WS_OVERLAPPEDWINDOW

void plat_set_window_scale(int scale)
{
    scale = UB_CLAMP(scale, 1, 3);
    g_fb.window_scale = scale;
    if (!g_hwnd) return;
    RECT r = { 0, 0, UB_FB_WIDTH * scale, UB_FB_HEIGHT * scale };
    AdjustWindowRect(&r, WINDOW_STYLE, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ub_info("display: window scale %dx", scale);
}

static bool create_window(HINSTANCE inst)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"UnibloxWindow";
    if (!RegisterClassW(&wc)) return false;

    DWORD style = WINDOW_STYLE;
    g_fb.window_scale = 2;
    RECT r = { 0, 0, UB_FB_WIDTH * 2, UB_FB_HEIGHT * 2 };
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowW(L"UnibloxWindow", L"Uniblox", style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, inst, NULL);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    g_input.has_focus = GetFocus() == g_hwnd;
    return true;
}

/* ----------------------------------------------------------------- main */

static int fatal(const WCHAR *what)
{
    char utf8[512];
    ub_utf16_to_utf8((const u16 *)what, utf8, sizeof utf8); /* WCHAR is 16-bit on Win32 */
    ub_error("fatal: %s", utf8);
    ub_log_close();
    MessageBoxW(NULL, what, L"Uniblox", MB_OK | MB_ICONSTOP);
    return 1;
}

static int win32_main(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    timeBeginPeriod(1); /* 1 ms Sleep() granularity for the frame limiter */
    win32_init_base_dir();
    ub_log_open("uniblox.log", "uniblox.old.log");

    ub_system_info sys;
    memset(&sys, 0, sizeof sys);
    query_system(&sys);
    {
        char base[MAX_PATH * 3];
        ub_utf16_to_utf8((const u16 *)win32_base_dir(), base, sizeof base);
        ub_info("running from %s", base[0] ? base : "(current directory)");
    }
    ub_info("%s, %u MB RAM, %d bpp display, mouse wheel %s", sys.os_name,
            (u32)(sys.total_phys_bytes >> 20), sys.display_bpp,
            sys.has_mouse_wheel ? "reported" : "not reported");

    g_msh_wheel_msg = RegisterWindowMessageW(L"MSWHEEL_ROLLMSG");

    ub_info("CPU: %s", sys.cpu_desc);
    if (!create_framebuffer(&sys)) return fatal(L"Out of memory allocating the framebuffer.");
    ub_info("display: %s%s", g_fb.format == UB_PIXEL_INDEX8 ? "8-bit palette output" : "32-bit colour output",
            g_palette_device ? " (palette device)" : "");
    if (!create_window(inst)) return fatal(L"Could not create the main window.");
    win32_audio_open(&sys);
    if (!app_init(&sys)) return fatal(L"Uniblox failed to initialise. See uniblox.log.");

    const u64 frame_us = 1000000u / 60u;
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

        u64 now = plat_time_us();
        u32 dt = (u32)(now - last);
        last = now;
        if (g_pending_format != g_fb.format) {
            g_fb.format = g_pending_format;
            ub_info("display: switched to %s output",
                    g_fb.format == UB_PIXEL_INDEX8 ? "8-bit palette" : "32-bit colour");
        }

        app_frame(&g_fb, &g_input, dt, &g_overlay);
        if (g_input.quit_requested) break;
        u64 t_present = plat_time_us();
        convert_framebuffer();
        HDC dc = GetDC(g_hwnd);
        present(dc);
        ReleaseDC(g_hwnd, dc);
        g_fb.present_us = (u32)(plat_time_us() - t_present);

        u64 spent = plat_time_us() - now;
        if (spent < frame_us) Sleep((DWORD)((frame_us - spent) / 1000u));
    }

    ub_info("shutting down");
    plat_set_mouse_capture(false);
    app_shutdown();
    win32_audio_close();
    ub_mixer_shutdown();
    timeEndPeriod(1);
    DestroyWindow(g_hwnd);
    if (g_hpal) DeleteObject(g_hpal);
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
