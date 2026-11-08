/* Uniblox - Win32 OpenGL context and UI font atlas.
 *
 * Nothing here is linked directly: opengl32.dll is loaded with
 * LoadLibraryA and the pixel-format functions are looked up in gdi32 at
 * run time, so the .exe still starts on systems without OpenGL (NT 3.51
 * without the OpenGL add-on, or a broken driver) and falls back to the
 * software renderer. */
#include "platform/win32/win32_internal.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "crt/ub_crt.h"

typedef HGLRC(WINAPI *pfn_wglCreateContext)(HDC);
typedef BOOL(WINAPI *pfn_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL(WINAPI *pfn_wglDeleteContext)(HGLRC);
typedef PROC(WINAPI *pfn_wglGetProcAddress)(LPCSTR);
typedef int(WINAPI *pfn_ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *);
typedef BOOL(WINAPI *pfn_SetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR *);
typedef int(WINAPI *pfn_DescribePixelFormat)(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
typedef BOOL(WINAPI *pfn_SwapBuffers)(HDC);
typedef BOOL(WINAPI *pfn_wglSwapIntervalEXT)(int);

static HMODULE g_gl_dll;
static HDC g_dc;
static HGLRC g_rc;
static pfn_wglCreateContext p_wglCreateContext;
static pfn_wglMakeCurrent p_wglMakeCurrent;
static pfn_wglDeleteContext p_wglDeleteContext;
static pfn_wglGetProcAddress p_wglGetProcAddress;
static pfn_SwapBuffers p_SwapBuffers;

void *plat_gl_proc(const char *name)
{
    if (!g_gl_dll) return NULL;
    void *p = p_wglGetProcAddress ? (void *)p_wglGetProcAddress(name) : NULL;
    /* some drivers return small integers or -1 instead of NULL */
    intptr_t v = (intptr_t)p;
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)
        p = (void *)GetProcAddress(g_gl_dll, name);
    return p;
}

/* Load an opengl32.dll and its wgl entry points; false (and unloaded) if
 * it cannot be loaded or is missing any of them. */
static bool load_gl_library(const char *path)
{
    g_gl_dll = LoadLibraryA(path);
    if (!g_gl_dll) {
        DWORD err = GetLastError();
        ub_warn("gl: could not load %s (error %u%s)", path, (u32)err,
                err == 127 ? ": it needs a newer version of Windows" :
                err == 126 ? ": not found" : "");
        return false;
    }
    p_wglCreateContext = (pfn_wglCreateContext)(void *)GetProcAddress(g_gl_dll, "wglCreateContext");
    p_wglMakeCurrent = (pfn_wglMakeCurrent)(void *)GetProcAddress(g_gl_dll, "wglMakeCurrent");
    p_wglDeleteContext = (pfn_wglDeleteContext)(void *)GetProcAddress(g_gl_dll, "wglDeleteContext");
    p_wglGetProcAddress = (pfn_wglGetProcAddress)(void *)GetProcAddress(g_gl_dll, "wglGetProcAddress");
    if (!p_wglCreateContext || !p_wglMakeCurrent || !p_wglDeleteContext || !p_wglGetProcAddress) {
        ub_warn("gl: %s is not a usable OpenGL library", path);
        FreeLibrary(g_gl_dll);
        g_gl_dll = NULL;
        return false;
    }
    return true;
}

bool win32_gl_init(HWND hwnd, bool vsync, bool *software_driver)
{
    *software_driver = false;
    /* Windows looks for DLLs in the program's folder first, so an
     * opengl32.dll copied there is tried before the system's. One built
     * for a newer Windows makes the loader pop up "Entry Point Not Found";
     * SetErrorMode suppresses that dialog, and we then try the system's
     * own copy by full path. */
    UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    bool loaded = load_gl_library("OPENGL32.DLL");
    if (!loaded) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        if (n > 0 && n < MAX_PATH - 16) {
            ub_fmt(path + n, MAX_PATH - (int)n, "\\OPENGL32.DLL");
            loaded = load_gl_library(path);
            if (loaded) ub_info("gl: using %s", path);
        }
    }
    SetErrorMode(old_mode);
    if (!loaded) return false;

    HMODULE gdi = GetModuleHandleA("GDI32.DLL");
    pfn_ChoosePixelFormat p_Choose = gdi ? (pfn_ChoosePixelFormat)(void *)GetProcAddress(gdi, "ChoosePixelFormat") : NULL;
    pfn_SetPixelFormat p_Set = gdi ? (pfn_SetPixelFormat)(void *)GetProcAddress(gdi, "SetPixelFormat") : NULL;
    pfn_DescribePixelFormat p_Describe = gdi ? (pfn_DescribePixelFormat)(void *)GetProcAddress(gdi, "DescribePixelFormat") : NULL;
    p_SwapBuffers = gdi ? (pfn_SwapBuffers)(void *)GetProcAddress(gdi, "SwapBuffers") : NULL;
    if (!p_Choose || !p_Set || !p_Describe || !p_SwapBuffers) {
        ub_warn("gl: this Windows has no OpenGL pixel-format functions");
        win32_gl_shutdown();
        return false;
    }

    g_dc = GetDC(hwnd); /* CS_OWNDC: this DC stays valid for the window's life */
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = p_Choose(g_dc, &pfd);
    if (!pf || !p_Set(g_dc, pf, &pfd)) {
        ub_warn("gl: no usable pixel format");
        win32_gl_shutdown();
        return false;
    }
    PIXELFORMATDESCRIPTOR got;
    memset(&got, 0, sizeof got);
    p_Describe(g_dc, pf, sizeof got, &got);
    /* Microsoft's own software OpenGL ("GDI Generic") is slower than our
     * software renderer; the caller decides whether to use it. */
    *software_driver = (got.dwFlags & PFD_GENERIC_FORMAT) && !(got.dwFlags & PFD_GENERIC_ACCELERATED);

    g_rc = p_wglCreateContext(g_dc);
    if (!g_rc || !p_wglMakeCurrent(g_dc, g_rc)) {
        ub_warn("gl: could not create a context");
        win32_gl_shutdown();
        return false;
    }
    pfn_wglSwapIntervalEXT swap_interval = (pfn_wglSwapIntervalEXT)plat_gl_proc("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(vsync ? 1 : 0);
    ub_info("gl: context created (%d-bit colour, %d-bit depth%s%s)", got.cColorBits, got.cDepthBits,
            *software_driver ? ", Microsoft software driver" : "", swap_interval ? "" : ", no vsync control");
    return true;
}

void win32_gl_swap(void)
{
    if (g_dc && p_SwapBuffers) p_SwapBuffers(g_dc);
}

void win32_gl_shutdown(void)
{
    if (g_rc) {
        p_wglMakeCurrent(NULL, NULL);
        p_wglDeleteContext(g_rc);
        g_rc = NULL;
    }
    g_dc = NULL;
    if (g_gl_dll) FreeLibrary(g_gl_dll);
    g_gl_dll = NULL;
}

/* ---------------------------------------------------------- font atlas */

static int next_pow2(int v)
{
    int p = 64;
    while (p < v) p *= 2;
    return p;
}

bool plat_font_atlas(int px_height, ub_font_atlas *out)
{
    memset(out, 0, sizeof *out);
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-px_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4 /* antialiased */,
                             DEFAULT_PITCH | FF_SWISS, L"Tahoma");
    HFONT old_font = (HFONT)SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    /* cells padded so no glyph can bleed into its neighbours */
    out->cell_w = tm.tmMaxCharWidth + 4;
    out->cell_h = tm.tmHeight + 4;
    out->glyph_h = tm.tmHeight + 1;
    out->line_h = tm.tmHeight + tm.tmExternalLeading;
    out->width = next_pow2(out->cell_w * 16);
    out->height = next_pow2(out->cell_h * 6);

    HBITMAP bmp = CreateCompatibleBitmap(screen, out->width, out->height);
    HBITMAP old_bmp = (HBITMAP)SelectObject(dc, bmp);
    PatBlt(dc, 0, 0, out->width, out->height, BLACKNESS);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 96; ++i) {
        WCHAR ch = (WCHAR)(32 + i);
        if (ch == 127) ch = L'?';
        SIZE sz;
        GetTextExtentPointW(dc, &ch, 1, &sz);
        out->advance[i] = (u8)UB_CLAMP(sz.cx, 1, 255);
        TextOutW(dc, (i % 16) * out->cell_w + 1, (i / 16) * out->cell_h + 1, &ch, 1);
    }
    SelectObject(dc, old_bmp); /* GetDIBits needs the bitmap deselected */

    bool ok = false;
    u32 *rgb = (u32 *)plat_alloc((size_t)out->width * out->height * 4);
    out->pixels = (u8 *)plat_alloc((size_t)out->width * out->height);
    if (rgb && out->pixels) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = out->width;
        bi.bmiHeader.biHeight = out->height; /* bottom-up */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(dc, bmp, 0, (UINT)out->height, rgb, &bi, DIB_RGB_COLORS)) {
            for (int y = 0; y < out->height; ++y) {
                const u32 *src = rgb + (size_t)(out->height - 1 - y) * out->width;
                u8 *dst = out->pixels + (size_t)y * out->width;
                for (int x = 0; x < out->width; ++x) {
                    u32 c = src[x];
                    u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                    u32 m = r > g ? r : g;
                    dst[x] = (u8)(m > b ? m : b); /* coverage, robust to ClearType tints */
                }
            }
            ok = true;
        }
    }
    plat_free(rgb);
    if (!ok) {
        plat_free(out->pixels);
        out->pixels = NULL;
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return ok;
}
