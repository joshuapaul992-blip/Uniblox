/* Uniblox - Win32 platform backend.
 *
 * Baseline target is Windows NT 3.51, so this file may only call APIs that
 * exist there. tools/checkpe.py enforces that every function the .exe
 * imports is on the vetted list in tools/nt351_imports.txt. Anything newer
 * (mouse wheel, Raw Input, GlobalMemoryStatusEx, Direct3D, ...) must be
 * looked up at runtime with GetProcAddress and have a fallback.
 *
 * Display path: the app renders 32-bit XRGB top-down; we convert to a
 * bottom-up 24-bit DIB (the format every Windows version supports), stretch
 * it into an off-screen bitmap, draw overlay text with GDI, then BitBlt to
 * the window so there is no flicker. */
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>

#include "platform/platform.h"
#include "app/app.h"
#include "core/ub_string.h"
#include "crt/ub_crt.h"

static HWND g_hwnd;
static HANDLE g_heap;
static LARGE_INTEGER g_qpf, g_qpc0;
static bool g_has_qpc;
static ub_input g_input;
static ub_overlay g_overlay;
static ub_framebuffer g_fb;
static u8 *g_dib;            /* 24bpp bottom-up copy of g_fb */
static int g_dib_pitch;      /* bytes, DWORD aligned */
static BITMAPINFO g_bmi;
static HDC g_memdc;
static HBITMAP g_membmp, g_oldbmp;
static int g_mem_w, g_mem_h;

/* ---------------------------------------------------------------- utils */

u64 plat_time_us(void)
{
    if (g_has_qpc) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        u64 ticks = (u64)(now.QuadPart - g_qpc0.QuadPart);
        u64 freq = (u64)g_qpf.QuadPart;
        return (ticks / freq) * 1000000u + ((ticks % freq) * 1000000u) / freq;
    }
    return (u64)GetTickCount() * 1000u;
}

void *plat_alloc(size_t bytes)
{
    return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, bytes);
}

void plat_free(void *p)
{
    if (p) HeapFree(g_heap, 0, p);
}

void plat_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    ub_vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

/* ASCII/UTF-8 -> UTF-16 without MultiByteToWideChar(CP_UTF8), which NT 3.51
 * lacks. Handles 1-3 byte sequences (the BMP); anything else becomes '?'. */
static int utf8_to_wide(const char *s, WCHAR *out, int cap)
{
    int n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n + 1 < cap) {
        u32 c = *p++;
        if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = ((c & 0x0F) << 12) | ((u32)(p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
            p += 1;
        } else if (c >= 0x80) {
            c = '?';
        }
        out[n++] = (WCHAR)c;
    }
    out[n] = 0;
    return n;
}

/* ---------------------------------------------------------- system info */

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
    ReleaseDC(NULL, dc);
}

/* ------------------------------------------------------------- display */

static bool create_framebuffer(void)
{
    g_fb.width = UB_FB_WIDTH;
    g_fb.height = UB_FB_HEIGHT;
    g_fb.pitch = UB_FB_WIDTH;
    g_fb.pixels = (u32 *)plat_alloc((size_t)g_fb.pitch * g_fb.height * 4);
    g_dib_pitch = (g_fb.width * 3 + 3) & ~3;
    g_dib = (u8 *)plat_alloc((size_t)g_dib_pitch * g_fb.height);
    if (!g_fb.pixels || !g_dib) return false;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = g_fb.width;
    g_bmi.bmiHeader.biHeight = g_fb.height; /* positive: bottom-up */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 24;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    return true;
}

static void convert_framebuffer(void)
{
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
    SetStretchBltMode(g_memdc, COLORONCOLOR);
    StretchDIBits(g_memdc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                  g_dib, &g_bmi, DIB_RGB_COLORS, SRCCOPY);

    SetBkMode(g_memdc, TRANSPARENT);
    for (int i = 0; i < g_overlay.count; ++i) {
        WCHAR wide[UB_OVERLAY_LINE_LEN];
        int n = utf8_to_wide(g_overlay.lines[i], wide, UB_OVERLAY_LINE_LEN);
        int y = 4 + i * 16;
        SetTextColor(g_memdc, RGB(0, 0, 0));
        TextOutW(g_memdc, 7, y + 1, wide, n);       /* drop shadow */
        SetTextColor(g_memdc, RGB(255, 255, 255));
        TextOutW(g_memdc, 6, y, wide, n);
    }

    BitBlt(wdc, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
}

/* --------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_input.quit_requested = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        g_input.key_down[wp & 0xFF] = true;
        if (wp == VK_ESCAPE) g_input.quit_requested = true;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        g_input.key_down[wp & 0xFF] = false;
        break;
    case WM_KILLFOCUS:
        memset(g_input.key_down, 0, sizeof g_input.key_down);
        break;
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        if (rc.right > 0 && rc.bottom > 0) {
            g_input.mouse_x = mx * g_fb.width / rc.right;
            g_input.mouse_y = my * g_fb.height / rc.bottom;
        }
        g_input.mouse_left = (wp & MK_LBUTTON) != 0;
        g_input.mouse_right = (wp & MK_RBUTTON) != 0;
    } break;
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

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, UB_FB_WIDTH * 2, UB_FB_HEIGHT * 2 };
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowW(L"UnibloxWindow", L"Uniblox", style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, inst, NULL);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    return true;
}

/* ----------------------------------------------------------------- main */

static int fatal(const WCHAR *what)
{
    MessageBoxW(NULL, what, L"Uniblox", MB_OK | MB_ICONSTOP);
    return 1;
}

static int win32_main(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    g_heap = GetProcessHeap();
    g_has_qpc = QueryPerformanceFrequency(&g_qpf) && g_qpf.QuadPart > 0;
    if (g_has_qpc) QueryPerformanceCounter(&g_qpc0);
    timeBeginPeriod(1); /* 1 ms Sleep() granularity for the frame limiter */

    ub_system_info sys;
    memset(&sys, 0, sizeof sys);
    query_system(&sys);

    if (!create_framebuffer()) return fatal(L"Out of memory allocating the framebuffer.");
    if (!create_window(inst)) return fatal(L"Could not create the main window.");
    if (!app_init(&sys)) return fatal(L"Uniblox failed to initialise.");

    const u64 frame_us = 1000000u / 60u;
    u64 last = plat_time_us();
    MSG msg;
    while (!g_input.quit_requested) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_input.quit_requested = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_input.quit_requested) break;

        u64 now = plat_time_us();
        u32 dt = (u32)(now - last);
        last = now;

        app_frame(&g_fb, &g_input, dt, &g_overlay);
        convert_framebuffer();
        HDC dc = GetDC(g_hwnd);
        present(dc);
        ReleaseDC(g_hwnd, dc);

        u64 spent = plat_time_us() - now;
        if (spent < frame_us) Sleep((DWORD)((frame_us - spent) / 1000u));
    }

    app_shutdown();
    timeEndPeriod(1);
    DestroyWindow(g_hwnd);
    return 0;
}

/* Process entry point (set with -e in the Makefile). There is no CRT, so
 * nothing runs before this: no static constructors, no argv parsing. */
void __cdecl ub_entry(void)
{
    ExitProcess((UINT)win32_main());
}
