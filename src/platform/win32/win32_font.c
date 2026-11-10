/* Uniblox - GUI glyphs from GDI (see plat_glyph in platform.h).
 *
 * A glyph is drawn white on black at four times its size (twice, or once,
 * for very large text) into a screen-compatible bitmap, read back with
 * GetDIBits and averaged down, so it comes out anti-aliased on every
 * Windows from NT 3.51 on, whatever font smoothing is set to. The baseline
 * is put on the supersampling grid, so glyphs sit on whole pixels.
 * Fonts are made once per face and size and kept (a few at a time). */
#include "platform/win32/win32_internal.h"
#include "core/ub_log.h"
#include "crt/ub_crt.h"

#ifndef NONANTIALIASED_QUALITY
#define NONANTIALIASED_QUALITY 3
#endif

static const WCHAR *const k_faces[UB_FACE_FAMILIES] = { L"Arial", L"Times New Roman", L"Courier New", L"Comic Sans MS" };

static HDC g_dc;
static HBITMAP g_bmp;
static int g_bw, g_bh;
static u32 *g_bits;
static int g_bits_cap;
static i8 g_have[UB_FACE_FAMILIES + 1]; /* -1 unknown, 0 missing, 1 present; [FAMILIES] = Arial Black */

typedef struct {
    u32 key;
    int height;
    HFONT font;
    int ascent, descent;
} fslot;
static fslot g_fonts[8];
static int g_next_font;

static bool face_is(HDC dc, const WCHAR *want)
{
    WCHAR face[64];
    int n = GetTextFaceW(dc, 64, face), i = 0;
    while (i < n && want[i] && face[i] == want[i]) ++i;
    return !want[i] && (i == n || face[i] == 0);
}

static bool ensure_dc(void)
{
    if (g_dc) return true;
    HDC screen = GetDC(NULL);
    g_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    return g_dc != NULL;
}

static HFONT make(const WCHAR *name, int height, int weight, bool italic)
{
    return CreateFontW(-height, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, name);
}

/* Is this face installed? (GDI maps a missing name to another face.) */
static bool have_face(int idx, const WCHAR *name)
{
    if (g_have[idx] >= 0) return g_have[idx] == 1;
    if (!ensure_dc()) return false;
    HFONT f = make(name, 32, FW_NORMAL, false);
    HFONT old = (HFONT)SelectObject(g_dc, f);
    g_have[idx] = face_is(g_dc, name) ? 1 : 0;
    SelectObject(g_dc, old);
    DeleteObject(f);
    static const char *const names[UB_FACE_FAMILIES + 1] = { "Arial", "Times New Roman", "Courier New", "Comic Sans MS",
                                                             "Arial Black" };
    if (idx != UB_FACE_SANS) ub_info("gui: %s %s", names[idx], g_have[idx] ? "found" : "missing (Arial instead)");
    return g_have[idx] == 1;
}

/* The font for a face at `height` pixels per em, selected into g_dc. */
static fslot *use_font(ub_typeface f, int height)
{
    static bool init;
    if (!init) {
        for (int i = 0; i <= UB_FACE_FAMILIES; ++i) g_have[i] = -1;
        init = true;
    }
    if (!ensure_dc()) return NULL;
    u32 key = (u32)f.family | ((u32)f.weight << 8) | ((u32)f.italic << 16) | 0x01000000u;
    for (int i = 0; i < 8; ++i)
        if (g_fonts[i].font && g_fonts[i].key == key && g_fonts[i].height == height) {
            SelectObject(g_dc, g_fonts[i].font);
            return &g_fonts[i];
        }
    int fam = f.family < UB_FACE_FAMILIES ? f.family : UB_FACE_SANS;
    const WCHAR *name = k_faces[fam];
    if (fam != UB_FACE_SANS && !have_face(fam, name)) name = k_faces[UB_FACE_SANS];
    int weight = f.weight <= 3 ? FW_LIGHT : f.weight >= 7 ? FW_BOLD : f.weight >= 6 ? FW_SEMIBOLD : FW_NORMAL;
    if (f.weight >= 8 && fam == UB_FACE_SANS) {
        if (have_face(UB_FACE_FAMILIES, L"Arial Black")) {
            name = L"Arial Black";
            weight = FW_HEAVY;
        } else {
            weight = FW_HEAVY;
        }
    }
    fslot *s = &g_fonts[g_next_font];
    g_next_font = (g_next_font + 1) % 8;
    if (s->font) DeleteObject(s->font);
    s->font = make(name, height, weight, f.italic != 0);
    s->key = key;
    s->height = height;
    SelectObject(g_dc, s->font);
    TEXTMETRICW tm;
    if (GetTextMetricsW(g_dc, &tm)) {
        s->ascent = tm.tmAscent;
        s->descent = tm.tmDescent;
    } else {
        s->ascent = height * 9 / 10;
        s->descent = height / 5;
    }
    return s;
}

bool plat_face_metrics(ub_typeface f, f32 *ascent, f32 *descent)
{
    fslot *s = use_font(f, 256);
    if (!s) return false;
    *ascent = (f32)s->ascent / 256.0f;
    *descent = (f32)s->descent / 256.0f;
    return true;
}

f32 plat_glyph_advance(ub_typeface f, u32 cp)
{
    if (cp > 0xFFFF) return 0.5f; /* outside the Basic Multilingual Plane: NT 3.51's GDI has no surrogates */
    fslot *s = use_font(f, 256);
    if (!s) return 0.5f;
    WCHAR ch = (WCHAR)cp;
    SIZE sz;
    if (!GetTextExtentPointW(g_dc, &ch, 1, &sz)) return 0.5f;
    return (f32)sz.cx / 256.0f;
}

static bool ensure_bitmap(int w, int h)
{
    if (g_bmp && w <= g_bw && h <= g_bh) return true;
    if (g_bmp) DeleteObject(g_bmp);
    int nw = w > g_bw ? w + 64 : g_bw, nh = h > g_bh ? h + 64 : g_bh;
    HDC screen = GetDC(NULL);
    g_bmp = CreateCompatibleBitmap(screen, nw, nh);
    ReleaseDC(NULL, screen);
    if (!g_bmp) {
        g_bw = g_bh = 0;
        return false;
    }
    g_bw = nw;
    g_bh = nh;
    if (g_bits_cap < nw * nh) {
        plat_free(g_bits);
        g_bits = (u32 *)plat_alloc(sizeof(u32) * (size_t)nw * (size_t)nh);
        g_bits_cap = g_bits ? nw * nh : 0;
    }
    return g_bits != NULL;
}

bool plat_glyph(ub_typeface f, u32 cp, f32 em_px, u8 *out, int cap, int *x0, int *y0, int *w, int *h)
{
    *x0 = *y0 = *w = *h = 0;
    if (cp == ' ' || cp == '\n' || cp == 0xA0 || cp > 0xFFFF) return true;
    int ss = em_px > 120 ? 1 : em_px > 60 ? 2 : 4; /* supersampling */
    int height = (int)(em_px * (f32)ss + 0.5f);
    if (height < 2) height = 2;
    fslot *s = use_font(f, height);
    if (!s) return false;
    WCHAR ch = (WCHAR)cp;
    SIZE sz;
    GetTextExtentPointW(g_dc, &ch, 1, &sz);
    /* room for overhangs (italics, wide glyphs) on both sides */
    int pad = ((height / 2 + 4) / ss + 1) * ss;
    int asc = s->ascent, top = pad + (ss - (pad + asc) % ss) % ss;
    int bw = ((sz.cx + pad * 2) / ss + 1) * ss, bh = ((top + asc + s->descent + pad) / ss + 1) * ss;
    if (!ensure_bitmap(bw, bh)) return false;
    HBITMAP old = (HBITMAP)SelectObject(g_dc, g_bmp);
    PatBlt(g_dc, 0, 0, bw, bh, BLACKNESS);
    SetTextColor(g_dc, RGB(255, 255, 255));
    SetBkMode(g_dc, TRANSPARENT);
    SelectObject(g_dc, s->font);
    TextOutW(g_dc, pad, top, &ch, 1);
    SelectObject(g_dc, old); /* GetDIBits needs the bitmap deselected */
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_bw;
    bi.bmiHeader.biHeight = -g_bh; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (!GetDIBits(g_dc, g_bmp, 0, (UINT)g_bh, g_bits, &bi, DIB_RGB_COLORS)) {
        /* some NT 3.51 drivers refuse top-down DIBs: read bottom-up and flip */
        bi.bmiHeader.biHeight = g_bh;
        if (!GetDIBits(g_dc, g_bmp, 0, (UINT)g_bh, g_bits, &bi, DIB_RGB_COLORS)) return false;
        for (int y = 0; y < g_bh / 2; ++y)
            for (int x = 0; x < g_bw; ++x) {
                u32 t = g_bits[y * g_bw + x];
                g_bits[y * g_bw + x] = g_bits[(g_bh - 1 - y) * g_bw + x];
                g_bits[(g_bh - 1 - y) * g_bw + x] = t;
            }
    }
    /* average ss x ss blocks; find the inked box */
    int ow = bw / ss, oh = bh / ss, area = ss * ss;
    int ix0 = ow, iy0 = oh, ix1 = -1, iy1 = -1;
    static u8 tmp[1024 * 1024];
    if (ow * oh > (int)sizeof tmp) return false;
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            u32 sum = 0;
            for (int j = 0; j < ss; ++j) {
                const u32 *row = g_bits + (size_t)(y * ss + j) * g_bw + x * ss;
                for (int i = 0; i < ss; ++i) {
                    u32 c = row[i], r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                    u32 m = r > g ? r : g;
                    sum += m > b ? m : b; /* robust to ClearType tints */
                }
            }
            u8 v = (u8)(sum / (u32)area);
            tmp[y * ow + x] = v;
            if (v) {
                if (x < ix0) ix0 = x;
                if (x > ix1) ix1 = x;
                if (y < iy0) iy0 = y;
                if (y > iy1) iy1 = y;
            }
        }
    if (ix1 < 0) return true; /* blank */
    int gw = ix1 - ix0 + 1, gh = iy1 - iy0 + 1;
    if (gw * gh > cap) return false;
    for (int y = 0; y < gh; ++y) memcpy(out + y * gw, tmp + (iy0 + y) * ow + ix0, (size_t)gw);
    *x0 = ix0 - pad / ss;
    *y0 = iy0 - (top + asc) / ss;
    *w = gw;
    *h = gh;
    return true;
}
