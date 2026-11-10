/* Uniblox - Win32 output for the software renderer.
 *
 * The app renders top-down into either a 32-bit XRGB or an 8-bit
 * palette-index buffer, normally smaller than the window (see
 * plat_set_sw_resolution). We copy that into a bottom-up DIB (24-bit, or
 * 8-bit with a colour table: formats every Windows version supports),
 * stretch it into an off-screen bitmap, draw the text queue with GDI at
 * window resolution (so text stays sharp), then BitBlt to the window.
 * With no text to draw, the image is stretched straight onto the window
 * instead, saving a full-window copy.
 *
 * Fastest path (32-bit colour, NT 3.5 and later): the frame is copied
 * into a DIB section, whose memory GDI shares with us, and blitted from a
 * memory DC. On NT 3.51 GDI runs in a separate server process (CSRSS), so
 * StretchDIBits must ship every frame's pixels across to it in chunks;
 * with a DIB section nothing crosses, which matters at 1080p and above.
 * GDI also queues drawing calls; GdiFlush() after each step makes the
 * timing breakdown honest.
 *
 * Otherwise, on 15/16/24/32-bit desktops a 32-bit BI_BITFIELDS DIB is tried first:
 * it is our own pixel layout, so no per-pixel conversion is needed and a
 * 32-bit desktop gets a straight copy. If the system refuses it (the first
 * StretchDIBits returns 0), we fall back to 24-bit for good.
 *
 * On 256-colour desktops we build a Windows palette from the app's colours
 * (keeping the 20 static system colours), realise it, and blit with
 * DIB_PAL_COLORS so pixels map straight to palette slots. */
#include "platform/win32/win32_internal.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

static ub_framebuffer g_fb;
static int g_alloc_pixels;   /* capacity of the buffers below, in pixels */
static u8 *g_dib;            /* 24bpp bottom-up copy (XRGB32 format) */
static u32 *g_dib32;         /* 32bpp bottom-up copy (XRGB32 format) */
static struct { BITMAPINFOHEADER h; DWORD masks[3]; } g_bmi32;
static int g_use32 = -1;     /* -1 untested, 0 refused, 1 works */

/* DIB section path (looked up at run time: NT 3.5+) */
typedef HBITMAP(WINAPI *pfn_CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
typedef BOOL(WINAPI *pfn_GdiFlush)(void);
static pfn_CreateDIBSection p_CreateDIBSection;
static pfn_GdiFlush p_GdiFlush;
static int g_use_section = -1;  /* -1 untested, 0 unavailable, 1 works */
static HDC g_secdc;
static HBITMAP g_secbmp, g_secold;
static u32 *g_secbits;
static int g_sec_w, g_sec_h;
static int g_dib_pitch;
static BITMAPINFO g_bmi;
static u8 *g_dib8;           /* 8bpp bottom-up copy (INDEX8 format) */
static int g_dib8_pitch;
static struct { BITMAPINFOHEADER h; RGBQUAD colors[256]; } g_bmi8rgb;
static struct { BITMAPINFOHEADER h; WORD index[256]; } g_bmi8pal;
static bool g_palette_device;
static HPALETTE g_hpal;
static u32 g_palette_serial_seen = 0xFFFFFFFFu;
static ub_pixel_format g_pending_format;
static int g_sw_height;      /* requested render height, 0 = window height */
static HDC g_memdc;
static HBITMAP g_membmp, g_oldbmp;
static int g_mem_w, g_mem_h;
static HFONT g_font;
static int g_font_px;

void plat_set_pixel_format(ub_pixel_format f)
{
    g_pending_format = f;
}

void plat_set_sw_resolution(int height)
{
    g_sw_height = height < 0 ? 0 : height;
}

static void gdi_flush(void)
{
    if (p_GdiFlush) p_GdiFlush();
}

static void free_section(void)
{
    if (g_secdc) {
        SelectObject(g_secdc, g_secold);
        DeleteDC(g_secdc);
        g_secdc = NULL;
    }
    if (g_secbmp) DeleteObject(g_secbmp);
    g_secbmp = NULL;
    g_secbits = NULL;
    g_sec_w = g_sec_h = 0;
}

/* (Re)create the DIB section for the current framebuffer size. */
static bool ensure_section(HDC wdc)
{
    if (g_use_section == 0) return false;
    if (g_secbmp && g_sec_w == g_fb.width && g_sec_h == g_fb.height) return true;
    free_section();
    void *bits = NULL;
    g_secbmp = p_CreateDIBSection ? p_CreateDIBSection(wdc, (BITMAPINFO *)&g_bmi32, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!g_secbmp || !bits) {
        if (g_secbmp) DeleteObject(g_secbmp);
        g_secbmp = NULL;
        g_use_section = 0;
        ub_info("display: DIB sections unavailable; using StretchDIBits");
        return false;
    }
    g_secbits = (u32 *)bits;
    g_secdc = CreateCompatibleDC(wdc);
    g_secold = (HBITMAP)SelectObject(g_secdc, g_secbmp);
    g_sec_w = g_fb.width;
    g_sec_h = g_fb.height;
    if (g_use_section < 0) ub_info("display: using a DIB section (frames are not copied to GDI)");
    g_use_section = 1;
    return true;
}

bool win32_sw_init(const ub_system_info *sys, int sw_height)
{
    HMODULE gdi = GetModuleHandleA("GDI32.DLL");
    if (gdi) {
        p_CreateDIBSection = (pfn_CreateDIBSection)(void *)GetProcAddress(gdi, "CreateDIBSection");
        p_GdiFlush = (pfn_GdiFlush)(void *)GetProcAddress(gdi, "GdiFlush");
    }
    if (!p_CreateDIBSection) g_use_section = 0;
    g_palette_device = sys->palette_display;
    g_fb.format = sys->display_bpp <= 8 ? UB_PIXEL_INDEX8 : UB_PIXEL_XRGB32;
    g_pending_format = g_fb.format;
    g_sw_height = sw_height;
    ub_info("display: software renderer, %s output%s",
            g_fb.format == UB_PIXEL_INDEX8 ? "8-bit palette" : "32-bit colour",
            g_palette_device ? " (palette device)" : "");
    return true;
}

static void free_buffers(void)
{
    plat_free(g_fb.pixels);
    plat_free(g_fb.pixels8);
    plat_free(g_dib);
    plat_free(g_dib8);
    plat_free(g_dib32);
    g_fb.pixels = NULL;
    g_fb.pixels8 = NULL;
    g_dib = g_dib8 = NULL;
    g_dib32 = NULL;
    g_alloc_pixels = 0;
}

/* Size the framebuffer for this frame from the window size. */
ub_framebuffer *win32_sw_prepare(int client_w, int client_h)
{
    int h = g_sw_height > 0 && g_sw_height < client_h ? g_sw_height : client_h;
    int w = (int)((i64)client_w * h / client_h);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w != g_fb.width || h != g_fb.height || !g_fb.pixels) {
        int dib_pitch = (w * 3 + 3) & ~3, dib8_pitch = (w + 3) & ~3;
        if (w * h > g_alloc_pixels || !g_fb.pixels) {
            free_buffers();
            g_fb.pixels = (u32 *)plat_alloc((size_t)w * h * 4);
            g_fb.pixels8 = (u8 *)plat_alloc((size_t)w * h);
            g_dib = (u8 *)plat_alloc((size_t)dib_pitch * h);
            g_dib8 = (u8 *)plat_alloc((size_t)dib8_pitch * h);
            g_dib32 = (u32 *)plat_alloc((size_t)w * h * 4);
            if (!g_fb.pixels || !g_fb.pixels8 || !g_dib || !g_dib8 || !g_dib32) {
                ub_error("display: out of memory for a %dx%d framebuffer", w, h);
                free_buffers();
                return NULL;
            }
            g_alloc_pixels = w * h;
        }
        g_fb.width = w;
        g_fb.height = h;
        g_fb.pitch = w;
        g_dib_pitch = dib_pitch;
        g_dib8_pitch = dib8_pitch;

        memset(&g_bmi, 0, sizeof g_bmi);
        g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        g_bmi.bmiHeader.biWidth = w;
        g_bmi.bmiHeader.biHeight = h; /* positive: bottom-up */
        g_bmi.bmiHeader.biPlanes = 1;
        g_bmi.bmiHeader.biBitCount = 24;
        g_bmi.bmiHeader.biCompression = BI_RGB;
        g_bmi8rgb.h = g_bmi.bmiHeader;
        g_bmi8rgb.h.biBitCount = 8;
        g_bmi8rgb.h.biClrUsed = 256;
        g_bmi8pal.h = g_bmi8rgb.h;
        for (int i = 0; i < 256; ++i) g_bmi8pal.index[i] = (WORD)i; /* identity */
        g_bmi32.h = g_bmi.bmiHeader;
        g_bmi32.h.biBitCount = 32;
        g_bmi32.h.biCompression = BI_BITFIELDS;
        g_bmi32.masks[0] = 0x00FF0000; /* red */
        g_bmi32.masks[1] = 0x0000FF00; /* green */
        g_bmi32.masks[2] = 0x000000FF; /* blue */
    }
    if (g_pending_format != g_fb.format) {
        g_fb.format = g_pending_format;
        ub_info("display: switched to %s output",
                g_fb.format == UB_PIXEL_INDEX8 ? "8-bit palette" : "32-bit colour");
    }
    return &g_fb;
}

/* Copy the app's top-down frame into the bottom-up DIB for its format. */
static void convert_framebuffer(void)
{
    if (g_fb.format == UB_PIXEL_XRGB32 && g_use32 != 0) {
        for (int y = 0; y < g_fb.height; ++y)
            memcpy(g_dib32 + (size_t)(g_fb.height - 1 - y) * g_fb.width,
                   g_fb.pixels + y * g_fb.pitch, (size_t)g_fb.width * 4);
        return;
    }
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

static void ensure_font(int px)
{
    if (g_font && px == g_font_px) return;
    if (g_font) DeleteObject(g_font);
    /* Tahoma where available (NT 4.0+); GDI substitutes a similar font on
     * NT 3.51. ANTIALIASED_QUALITY (4) is ignored where unsupported. */
    g_font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4, DEFAULT_PITCH | FF_SWISS,
                         L"Tahoma");
    g_font_px = px;
}

/* Stretch the frame onto `dc` in whichever DIB format applies. */
static void stretch_frame(HDC dc, int w, int h)
{
    SetStretchBltMode(dc, COLORONCOLOR);
    if (g_fb.format == UB_PIXEL_INDEX8) {
        if (g_hpal)
            StretchDIBits(dc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                          g_dib8, (BITMAPINFO *)&g_bmi8pal, DIB_PAL_COLORS, SRCCOPY);
        else
            StretchDIBits(dc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                          g_dib8, (BITMAPINFO *)&g_bmi8rgb, DIB_RGB_COLORS, SRCCOPY);
        return;
    }
    if (g_use32 != 0) {
        int lines = StretchDIBits(dc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height,
                                  g_dib32, (BITMAPINFO *)&g_bmi32, DIB_RGB_COLORS, SRCCOPY);
        if (g_use32 < 0) {
            g_use32 = lines > 0 && lines != (int)GDI_ERROR;
            ub_info("display: 32-bit DIBs %s", g_use32 ? "supported (no colour conversion)"
                                                       : "refused; using 24-bit");
            if (!g_use32) convert_framebuffer(); /* redo this frame in 24-bit */
        }
        if (g_use32) return;
    }
    StretchDIBits(dc, 0, 0, w, h, 0, 0, g_fb.width, g_fb.height, g_dib, &g_bmi, DIB_RGB_COLORS,
                  SRCCOPY);
}

static void draw_text_queue(HDC dc, const ub_frame *frame)
{
    ensure_font(frame->text_px > 6 ? frame->text_px : 13);
    HFONT old = g_font ? (HFONT)SelectObject(dc, g_font) : NULL;
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < frame->text_count; ++i) {
        const ub_text_item *t = &frame->text[i];
        u16 wide[UB_TEXT_ITEM_LEN];
        int n = ub_utf8_to_utf16(t->text, wide, UB_TEXT_ITEM_LEN);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutW(dc, t->x + 1, t->y + 1, (LPCWSTR)wide, n);   /* drop shadow */
        SetTextColor(dc, RGB((t->color >> 16) & 255, (t->color >> 8) & 255, t->color & 255));
        TextOutW(dc, t->x, t->y, (LPCWSTR)wide, n);
    }
    if (old) SelectObject(dc, old);
}

void win32_sw_present(HDC wdc, ub_frame *frame)
{
    int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0 || !g_fb.pixels) return;
    u64 t0 = plat_time_us(), t1, t2, t3;
    update_palette(wdc);
    if (g_hpal) {
        SelectPalette(wdc, g_hpal, FALSE);
        RealizePalette(wdc);
    }
    bool one_to_one = g_fb.width == w && g_fb.height == h;
    const char *path;

    if (g_fb.format == UB_PIXEL_XRGB32 && ensure_section(wdc)) {
        gdi_flush(); /* GDI must be done with the section before we write it */
        for (int y = 0; y < g_fb.height; ++y)
            memcpy(g_secbits + (size_t)(g_fb.height - 1 - y) * g_fb.width,
                   g_fb.pixels + y * g_fb.pitch, (size_t)g_fb.width * 4);
        t1 = plat_time_us();
        if (one_to_one) {
            /* text straight onto the section: one blit, still sharp */
            if (frame->text_count) draw_text_queue(g_secdc, frame);
            gdi_flush();
            t2 = plat_time_us();
            BitBlt(wdc, 0, 0, w, h, g_secdc, 0, 0, SRCCOPY);
        } else if (frame->text_count) {
            ensure_backbuffer(wdc, w, h);
            SetStretchBltMode(g_memdc, COLORONCOLOR);
            StretchBlt(g_memdc, 0, 0, w, h, g_secdc, 0, 0, g_fb.width, g_fb.height, SRCCOPY);
            draw_text_queue(g_memdc, frame);
            gdi_flush();
            t2 = plat_time_us();
            BitBlt(wdc, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
        } else {
            t2 = plat_time_us();
            SetStretchBltMode(wdc, COLORONCOLOR);
            StretchBlt(wdc, 0, 0, w, h, g_secdc, 0, 0, g_fb.width, g_fb.height, SRCCOPY);
        }
        gdi_flush();
        t3 = plat_time_us();
        path = "DIB section";
    } else {
        convert_framebuffer();
        t1 = plat_time_us();
        if (frame->text_count == 0) {
            /* nothing to overlay: one stretch straight onto the window */
            t2 = t1;
            stretch_frame(wdc, w, h);
        } else {
            ensure_backbuffer(wdc, w, h);
            if (g_hpal) {
                SelectPalette(g_memdc, g_hpal, FALSE);
                RealizePalette(g_memdc);
            }
            stretch_frame(g_memdc, w, h);
            draw_text_queue(g_memdc, frame);
            gdi_flush();
            t2 = plat_time_us();
            BitBlt(wdc, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
        }
        gdi_flush();
        t3 = plat_time_us();
        path = g_fb.format == UB_PIXEL_INDEX8 ? "8-bit DIB" : g_use32 > 0 ? "32-bit DIB" : "24-bit DIB";
    }
    u32 c = (u32)(t1 - t0) / 100u, st = (u32)(t2 - t1) / 100u, b = (u32)(t3 - t2) / 100u;
    ub_fmt(frame->present_detail, sizeof frame->present_detail,
           "copy %u.%u + %s %u.%u + blit %u.%u ms (%s, %dx%d -> %dx%d)", c / 10, c % 10,
           one_to_one ? "text" : frame->text_count ? "stretch+text" : "-", st / 10, st % 10,
           b / 10, b % 10, path, g_fb.width, g_fb.height, w, h);
}

/* WM_QUERYNEWPALETTE / WM_PALETTECHANGED; returns true if handled. */
bool win32_sw_palette_message(HWND hwnd, UINT msg, WPARAM wp, LRESULT *result)
{
    if (!g_hpal) return false;
    if (msg == WM_QUERYNEWPALETTE) { /* becoming active: claim the palette */
        HDC dc = GetDC(hwnd);
        SelectPalette(dc, g_hpal, FALSE);
        UINT changed = RealizePalette(dc);
        ReleaseDC(hwnd, dc);
        if (changed) InvalidateRect(hwnd, NULL, FALSE);
        *result = TRUE;
        return true;
    }
    if (msg == WM_PALETTECHANGED && (HWND)wp != hwnd) {
        HDC dc = GetDC(hwnd);
        SelectPalette(dc, g_hpal, FALSE);
        RealizePalette(dc);
        ReleaseDC(hwnd, dc);
        InvalidateRect(hwnd, NULL, FALSE);
        *result = 0;
        return true;
    }
    return false;
}

void win32_sw_shutdown(void)
{
    if (g_memdc) {
        SelectObject(g_memdc, g_oldbmp);
        DeleteObject(g_membmp);
        DeleteDC(g_memdc);
        g_memdc = NULL;
    }
    if (g_font) DeleteObject(g_font);
    g_font = NULL;
    free_section();
    if (g_hpal) DeleteObject(g_hpal);
    g_hpal = NULL;
    free_buffers();
}
