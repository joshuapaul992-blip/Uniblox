/* Uniblox - tests for the GUI: the rasteriser, PNG/BMP decoding, text
 * layout, Roblox's layout rules, drawing and input. Text uses the stub
 * platform's box glyphs (half an em wide, spaces a quarter), so sizes can
 * be checked exactly. */
#include <math.h>
#include <stdlib.h>

#include "core/ub_compress.h"
#include "gui/ub_canvas.h"
#include "gui/ub_gui.h"
#include "gui/ub_image.h"
#include "gui/ub_text.h"
#include "platform/platform.h"
#include "world/ub_classes.h"
#include "test.h"

/* ------------------------------------------------------------ canvas */

static u32 px_at(const ub_canvas *c, int x, int y) { return c->px[y * c->pitch + x]; }

static void test_canvas(void)
{
    static u32 buf[64 * 64];
    memset(buf, 0, sizeof buf);
    ub_canvas c;
    ub_canvas_init(&c, buf, 64, 64, 64);
    ub_paint p;
    memset(&p, 0, sizeof p);
    p.color = 0xFF0000;
    p.alpha = 1;
    ub_xform t = ub_xform_place(10, 10);
    ub_canvas_rect(&c, &t, 20, 10, 0, &p);
    CHECK(px_at(&c, 10, 10) == 0xFFFF0000u && px_at(&c, 29, 19) == 0xFFFF0000u);
    CHECK(px_at(&c, 9, 10) == 0 && px_at(&c, 30, 10) == 0 && px_at(&c, 10, 20) == 0);
    /* half transparent blue over it: premultiplied */
    p.color = 0x0000FF;
    p.alpha = 0.5f;
    ub_canvas_rect(&c, &t, 1, 1, 0, &p);
    u32 v = px_at(&c, 10, 10);
    CHECK((v >> 24) == 255 && ((v >> 16) & 255) >= 126 && ((v >> 16) & 255) <= 129 && (v & 255) >= 126 && (v & 255) <= 129);
    /* rounded corners: the corner pixel stays empty, the middle is full */
    memset(buf, 0, sizeof buf);
    p.color = 0x00FF00;
    p.alpha = 1;
    t = ub_xform_place(4, 4);
    ub_canvas_rect(&c, &t, 40, 40, 10, &p);
    CHECK(px_at(&c, 4, 4) == 0 && px_at(&c, 24, 24) == 0xFF00FF00u && (px_at(&c, 24, 4) >> 24) == 255);
    u32 edge = px_at(&c, 6, 7) >> 24; /* on the curve: partly covered */
    CHECK(edge > 0 && edge < 255);
    /* a ring leaves the middle alone */
    memset(buf, 0, sizeof buf);
    ub_canvas_ring(&c, &t, 40, 40, 0, 2, 0, &p);
    CHECK(px_at(&c, 4, 4) == 0xFF00FF00u && px_at(&c, 5, 20) == 0xFF00FF00u && px_at(&c, 20, 20) == 0);
    /* clipping */
    memset(buf, 0, sizeof buf);
    ub_canvas_clip(&c, 0, 0, 16, 64);
    ub_canvas_rect(&c, &t, 40, 40, 0, &p);
    CHECK(px_at(&c, 15, 20) == 0xFF00FF00u && px_at(&c, 16, 20) == 0);
    ub_canvas_clip(&c, 0, 0, 64, 64);
    /* a gradient from red to blue, left to right */
    memset(buf, 0, sizeof buf);
    static const f32 keys[8] = { 0, 1, 0, 0, 1, 0, 0, 1 };
    p.color = 0xFFFFFF;
    p.gcol = keys;
    p.gcol_n = 2;
    p.gdx = 1;
    p.gdy = 0;
    t = ub_xform_place(0, 0);
    ub_canvas_rect(&c, &t, 64, 8, 0, &p);
    u32 l = px_at(&c, 0, 4), r = px_at(&c, 63, 4);
    CHECK(((l >> 16) & 255) > 240 && (l & 255) < 10 && ((r >> 16) & 255) < 10 && (r & 255) > 240);
    /* a rotated square still covers its centre, and reaches past its unrotated corners' edges */
    memset(buf, 0, sizeof buf);
    memset(&p, 0, sizeof p);
    p.color = 0xFFFFFF;
    p.alpha = 1;
    t = ub_xform_rotated(22, 22, 20, 20, 45);
    ub_canvas_rect(&c, &t, 20, 20, 0, &p);
    CHECK(px_at(&c, 32, 32) == 0xFFFFFFFFu && px_at(&c, 22, 22) == 0 && px_at(&c, 32, 19) != 0);
}

/* --------------------------------------------------------- PNG and BMP */

typedef struct {
    u8 *p;
    u32 n, cap;
} obuf;

static void put(obuf *b, const void *d, u32 n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = (u8 *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put32(obuf *b, u32 v)
{
    u8 x[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    put(b, x, 4);
}

static u32 crc32(const u8 *d, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void chunk(obuf *b, const char *type, const u8 *d, u32 n)
{
    put32(b, n);
    u32 at = b->n;
    put(b, type, 4);
    if (n) put(b, d, n);
    put32(b, crc32(b->p + at, n + 4));
}

/* zlib with stored blocks */
static void zlib_stored(obuf *z, const u8 *d, u32 n)
{
    u8 h[2] = { 0x78, 0x01 };
    put(z, h, 2);
    u32 pos = 0;
    do {
        u32 len = n - pos > 65535 ? 65535 : n - pos;
        u8 bh[5] = { (u8)(pos + len == n), (u8)len, (u8)(len >> 8), (u8)~len, (u8)(~len >> 8) };
        put(z, bh, 5);
        put(z, d + pos, len);
        pos += len;
    } while (pos < n);
    u32 a = 1, s = 0;
    for (u32 i = 0; i < n; ++i) {
        a = (a + d[i]) % 65521;
        s = (s + a) % 65521;
    }
    put32(z, (s << 16) | a);
}

/* Filters one row in place from its raw bytes (filter types 0..4 in turn). */
static void filter_row(u8 *out, const u8 *row, const u8 *prev, u32 n, u32 bpp, int f)
{
    out[0] = (u8)f;
    for (u32 i = 0; i < n; ++i) {
        int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0, pr = 0;
        if (f == 1) pr = a;
        else if (f == 2) pr = b;
        else if (f == 3) pr = (a + b) / 2;
        else if (f == 4) {
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
        out[1 + i] = (u8)(row[i] - pr);
    }
}

/* An 8-bit RGBA PNG of the pixels (0xAARRGGBB, not premultiplied). */
static u8 *make_png(const u32 *px, u32 w, u32 h, bool interlace, u32 *size)
{
    obuf raw = { 0 }, z = { 0 }, f = { 0 };
    static const u8 x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const u8 dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1, ftype = 0;
    for (int p = 0; p < passes; ++p) {
        u32 pw = interlace ? (w > x0[p] ? (w - x0[p] + dx[p] - 1) / dx[p] : 0) : w;
        u32 ph = interlace ? (h > y0[p] ? (h - y0[p] + dy[p] - 1) / dy[p] : 0) : h;
        if (!pw || !ph) continue;
        u8 *prev = NULL, *row = (u8 *)malloc(pw * 4), *last = (u8 *)malloc(pw * 4), *out = (u8 *)malloc(pw * 4 + 1);
        for (u32 y = 0; y < ph; ++y) {
            for (u32 x = 0; x < pw; ++x) {
                u32 c = px[(interlace ? y0[p] + y * dy[p] : y) * w + (interlace ? x0[p] + x * dx[p] : x)];
                row[x * 4] = (u8)(c >> 16);
                row[x * 4 + 1] = (u8)(c >> 8);
                row[x * 4 + 2] = (u8)c;
                row[x * 4 + 3] = (u8)(c >> 24);
            }
            filter_row(out, row, prev, pw * 4, 4, ftype++ % 5);
            put(&raw, out, pw * 4 + 1);
            memcpy(last, row, pw * 4);
            prev = last;
        }
        free(row);
        free(last);
        free(out);
    }
    zlib_stored(&z, raw.p, raw.n);
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    put(&f, sig, 8);
    u8 ihdr[13] = { (u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h,
                    8, 6, 0, 0, (u8)(interlace ? 1 : 0) };
    chunk(&f, "IHDR", ihdr, 13);
    chunk(&f, "IDAT", z.p, z.n);
    chunk(&f, "IEND", NULL, 0);
    free(raw.p);
    free(z.p);
    *size = f.n;
    return f.p;
}

static u32 premul(u32 c)
{
    u32 a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (a == 255) return c;
    return (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
}

static void test_images(void)
{
    u32 w = 13, h = 9, src[13 * 9];
    u32 seed = 99;
    for (u32 i = 0; i < w * h; ++i) {
        seed = seed * 1664525u + 1013904223u;
        src[i] = seed;
    }
    src[0] = 0xFFFFFFFFu;
    src[1] = 0x00000000u;
    for (int il = 0; il < 2; ++il) {
        u32 size;
        u8 *png = make_png(src, w, h, il != 0, &size);
        ub_image_px img;
        CHECK(ub_image_decode(png, size, &img) && img.w == (int)w && img.h == (int)h);
        int bad = 0;
        for (u32 i = 0; img.px && i < w * h; ++i) bad += img.px[i] != premul(src[i]);
        CHECK(bad == 0);
        if (bad) printf("    %s PNG: %d wrong pixels\n", il ? "interlaced" : "plain", bad);
        plat_free((void *)img.px);
        /* damaged files are refused or decoded harmlessly (ASan watches) */
        for (int t = 0; t < 300; ++t) {
            u8 *b = (u8 *)malloc(size);
            memcpy(b, png, size);
            seed = seed * 1664525u + 1013904223u;
            b[(seed >> 8) % size] ^= (u8)(1 + (seed & 0x7F));
            u32 len = (t & 3) == 0 ? (seed >> 4) % size : size;
            if (ub_image_decode(b, len, &img)) plat_free((void *)img.px);
            free(b);
        }
        free(png);
    }
    /* a 24-bit BMP, bottom-up */
    obuf bm = { 0 };
    u32 stride = ((w * 3 + 3) / 4) * 4, data = stride * h;
    u8 hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    u32 total = 54 + data;
    memcpy(hdr + 2, &total, 4);
    u32 off = 54, hs = 40, bw = w, bh = h;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &hs, 4);
    memcpy(hdr + 18, &bw, 4);
    memcpy(hdr + 22, &bh, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    put(&bm, hdr, 54);
    for (u32 y = 0; y < h; ++y) {
        u8 row[64];
        memset(row, 0, sizeof row);
        for (u32 x = 0; x < w; ++x) {
            u32 c = src[(h - 1 - y) * w + x];
            row[x * 3] = (u8)c;
            row[x * 3 + 1] = (u8)(c >> 8);
            row[x * 3 + 2] = (u8)(c >> 16);
        }
        put(&bm, row, stride);
    }
    ub_image_px img;
    CHECK(ub_image_decode(bm.p, bm.n, &img) && img.w == (int)w && img.h == (int)h);
    int bad = 0;
    for (u32 i = 0; img.px && i < w * h; ++i) bad += img.px[i] != (src[i] | 0xFF000000u);
    CHECK(bad == 0);
    plat_free((void *)img.px);
    free(bm.p);
    /* content ids name local files only */
    char path[128];
    CHECK(ub_image_path("rbxassetid://12345", path, sizeof path, "png") && !strcmp(path, "content/12345.png"));
    CHECK(ub_image_path("http://www.roblox.com/asset/?id=777", path, sizeof path, "bmp") && !strcmp(path, "content/777.bmp"));
    CHECK(ub_image_path("rbxasset://textures/ui/x.png", path, sizeof path, "png") && !strcmp(path, "content/textures/ui/x.png"));
    CHECK(!ub_image_path("rbxasset://../../secret.png", path, sizeof path, "png"));
    CHECK(!ub_image_path("rbxthumb://type=AvatarHeadShot&id=1", path, sizeof path, "png"));
}

/* -------------------------------------------------------------- text */

static void lay(const char *text, f32 size, f32 bw, f32 bh, bool wrap, bool rich, ub_text_layout *l)
{
    ub_text_style s;
    memset(&s, 0, sizeof s);
    s.text = text;
    s.len = (u32)strlen(text);
    s.rich = rich;
    s.base.face = ub_text_face_enum(3);
    s.base.size = size;
    s.line_height = 1;
    s.box_w = bw;
    s.box_h = bh;
    s.xalign = 0;
    s.yalign = 0;
    s.wrap = wrap;
    ub_text_lay_out(&s, l);
}

static void test_text(void)
{
    ub_text_layout l;
    ub_text_layout_init(&l);
    /* size 20: 20 px per em (ascent + descent = 1 em), so 'a' is 10 wide and ' ' 5 */
    lay("ab cd", 20, 1000, 100, false, false, &l);
    CHECK(fabsf(l.bounds_w - 45) < 0.01f && fabsf(l.bounds_h - 20) < 0.01f && l.lines == 1);
    lay("ab cd", 20, 30, 100, true, false, &l);
    CHECK(l.lines == 2 && fabsf(l.bounds_w - 20) < 0.01f && fabsf(l.bounds_h - 40) < 0.01f);
    CHECK(l.n == 5 && l.g[3].x == 0 && l.g[3].y > 20); /* "cd" starts the second line */
    lay("one\ntwo\n", 10, 1000, 100, false, false, &l);
    CHECK(l.lines == 3 && fabsf(l.bounds_h - 30) < 0.01f);
    /* rich text: bold is wider, entities are characters, tags are not */
    lay("<b>aa</b>a &lt;", 20, 1000, 100, false, true, &l);
    CHECK(l.n == 5 && l.g[4].cp == '<' && fabsf(l.bounds_w - (12.5f * 2 + 10 + 5 + 10)) < 0.01f);
    lay("<font size=\"40\" color=\"#FF0000\">A</font>b", 20, 1000, 100, false, true, &l);
    CHECK(l.n == 2 && l.runs[l.g[0].run].size == 40 && l.runs[l.g[0].run].color == 0xFF0000 &&
          fabsf(l.bounds_h - 40) < 0.01f);
    lay("<b>unclosed", 20, 1000, 100, false, true, &l);
    CHECK(l.n == 8);
    lay("not <a tag>", 20, 1000, 100, false, true, &l); /* unknown tags are shown as written */
    CHECK(l.n == 11);
    /* TextScaled: the biggest size that fits */
    ub_text_style s;
    memset(&s, 0, sizeof s);
    s.text = "abcd";
    s.len = 4;
    s.base.face = ub_text_face_enum(3);
    s.base.size = 8;
    s.line_height = 1;
    s.box_w = 100;
    s.box_h = 30;
    f32 got = ub_text_lay_out_scaled(&s, 1, 100, &l);
    CHECK(got == 30); /* 4 x 15 = 60 wide, 30 tall */
    s.box_h = 200;
    got = ub_text_lay_out_scaled(&s, 1, 100, &l);
    CHECK(got == 50); /* width-bound: 4 x 25 = 100 */
    /* truncation ends with dots that fit */
    memset(&s, 0, sizeof s);
    s.text = "abcdefghij";
    s.len = 10;
    s.base.face = ub_text_face_enum(3);
    s.base.size = 20;
    s.line_height = 1;
    s.box_w = 60;
    s.box_h = 20;
    s.truncate = true;
    ub_text_lay_out(&s, &l);
    CHECK(l.n >= 4 && l.g[l.n - 1].cp == '.' && l.bounds_w <= 60.01f);
    f32 mw, mh;
    ub_text_measure("hello", 5, ub_text_face_enum(3), 14, 0, &mw, &mh);
    CHECK(fabsf(mw - 35) < 0.01f && fabsf(mh - 14) < 0.01f);
    ub_text_layout_free(&l);
}

/* ---------------------------------------------------------- the engine */

static i32 make(ub_world *w, i32 parent, const char *cls, const char *name)
{
    i32 i = ub_world_add(w, parent, UB_CLASS_OTHER, cls, name);
    ub_cdb_fill_defaults(w, i, cls);
    return i;
}

static void set_ud2(ub_world *w, i32 i, const char *k, f32 sx, i32 ox, f32 sy, i32 oy)
{
    ub_value v;
    v.type = UB_PV_UDIM2;
    v.v.ud2.sx = sx;
    v.v.ud2.ox = ox;
    v.v.ud2.sy = sy;
    v.v.ud2.oy = oy;
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_v2(ub_world *w, i32 i, const char *k, f32 x, f32 y)
{
    ub_value v;
    v.type = UB_PV_VECTOR2;
    v.v.v2.x = x;
    v.v.v2.y = y;
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_ud(ub_world *w, i32 i, const char *k, f32 s, i32 o)
{
    ub_value v;
    v.type = UB_PV_UDIM;
    v.v.ud.s = s;
    v.v.ud.o = o;
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_num(ub_world *w, i32 i, const char *k, f32 f)
{
    ub_value v = ub_vfloat(f);
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_int(ub_world *w, i32 i, const char *k, i32 n)
{
    ub_value v = ub_vint(n);
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_enum(ub_world *w, i32 i, const char *k, u32 e)
{
    ub_value v = ub_venum(e);
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_bool(ub_world *w, i32 i, const char *k, bool b)
{
    ub_value v = ub_vbool(b);
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_color(ub_world *w, i32 i, const char *k, f32 r, f32 g, f32 b)
{
    ub_value v;
    v.type = UB_PV_COLOR3;
    v.v.c3.r = r;
    v.v.c3.g = g;
    v.v.c3.b = b;
    ub_world_set_prop_named(w, i, k, &v);
}

static void set_str(ub_world *w, i32 i, const char *k, const char *s)
{
    ub_value v = ub_vstr(s, (u32)strlen(s));
    ub_world_set_prop_named(w, i, k, &v);
}

static bool near_rect(ub_gui *g, ub_world *w, i32 i, f32 x, f32 y, f32 ww, f32 hh)
{
    ub_gui_info in;
    if (!ub_gui_info_of(g, w, i, &in)) return false;
    bool ok = fabsf(in.x - x) < 0.01f && fabsf(in.y - y) < 0.01f && fabsf(in.w - ww) < 0.01f && fabsf(in.h - hh) < 0.01f;
    if (!ok) printf("    instance %d: (%g, %g) %gx%g, wanted (%g, %g) %gx%g\n", i, in.x, in.y, in.w, in.h, x, y, ww, hh);
    return ok;
}

static int count_events(ub_gui *g, i32 inst, u8 type)
{
    const ub_gui_event *e;
    int n = ub_gui_events(g, &e), c = 0;
    for (int i = 0; i < n; ++i) c += e[i].inst == inst && e[i].type == type;
    return c;
}

static void test_layout(void)
{
    ub_world w;
    ub_world_init(&w);
    i32 pg = make(&w, -1, "PlayerGui", "PlayerGui");
    i32 sg = make(&w, pg, "ScreenGui", "Hud");
    /* the screen is 800x636: GUI coordinates are 800x600 below the 36-pixel inset */
    i32 f = make(&w, sg, "Frame", "Panel");
    set_ud2(&w, f, "Size", 0.5f, 10, 0, 50);
    set_ud2(&w, f, "Position", 0.5f, 0, 1, -10);
    set_v2(&w, f, "AnchorPoint", 0.5f, 1);
    ub_gui *g = ub_gui_new();
    ub_gui_set_root(g, pg);
    ub_gui_update(g, &w, 800, 636, NULL, NULL, 0);
    CHECK(near_rect(g, &w, f, 400 - 205, 600 - 10 - 50, 410, 50));
    /* IgnoreGuiInset: the same frame sits 36 pixels higher in GUI coordinates */
    set_bool(&w, sg, "IgnoreGuiInset", true);
    CHECK(near_rect(g, &w, f, 400 - 205, 636 - 36 - 10 - 50, 410, 50));
    set_bool(&w, sg, "IgnoreGuiInset", false);

    /* a list: three 100x20 rows with 5 pixels between, centred across */
    i32 list = make(&w, f, "UIListLayout", "List");
    set_ud(&w, list, "Padding", 0, 5);
    set_enum(&w, list, "HorizontalAlignment", 0); /* Center */
    set_enum(&w, list, "SortOrder", 2);           /* LayoutOrder */
    i32 rows[3];
    for (int i = 0; i < 3; ++i) {
        char nm[8] = { 'R', (char)('0' + i), 0 };
        rows[i] = make(&w, f, "TextLabel", nm);
        set_ud2(&w, rows[i], "Size", 0, 100, 0, 20);
        set_int(&w, rows[i], "LayoutOrder", 3 - i); /* in reverse */
    }
    f32 fx = 400 - 205, fy = 540;
    CHECK(near_rect(g, &w, rows[2], fx + 155, fy, 100, 20));
    CHECK(near_rect(g, &w, rows[1], fx + 155, fy + 25, 100, 20));
    CHECK(near_rect(g, &w, rows[0], fx + 155, fy + 50, 100, 20));
    ub_gui_info in;
    CHECK(ub_gui_info_of(g, &w, list, &in) && in.content_w == 100 && in.content_h == 70);
    /* padding moves the content */
    i32 pad = make(&w, f, "UIPadding", "Pad");
    set_ud(&w, pad, "PaddingTop", 0, 4);
    set_ud(&w, pad, "PaddingLeft", 0, 10);
    CHECK(near_rect(g, &w, rows[2], fx + 10 + (400 - 100) * 0.5f, fy + 4, 100, 20));
    ub_world_destroy(&w, pad);
    ub_world_destroy(&w, list);
    /* without a layout the rows go back to their Position (0, 0) */
    CHECK(near_rect(g, &w, rows[0], fx, fy, 100, 20));

    /* a grid: 60x40 cells, 10 apart, in a 200-wide frame: three across */
    i32 gf = make(&w, sg, "Frame", "Grid");
    set_ud2(&w, gf, "Size", 0, 200, 0, 200);
    i32 grid = make(&w, gf, "UIGridLayout", "G");
    set_ud2(&w, grid, "CellSize", 0, 60, 0, 40);
    set_ud2(&w, grid, "CellPadding", 0, 10, 0, 10);
    i32 cells[5];
    for (int i = 0; i < 5; ++i) {
        char nm[8] = { 'C', (char)('0' + i), 0 };
        cells[i] = make(&w, gf, "Frame", nm);
    }
    CHECK(near_rect(g, &w, cells[0], 0, 0, 60, 40) && near_rect(g, &w, cells[2], 140, 0, 60, 40) &&
          near_rect(g, &w, cells[3], 0, 50, 60, 40));
    CHECK(ub_gui_info_of(g, &w, grid, &in) && in.cells_x == 3 && in.cells_y == 2 && in.content_w == 200);

    /* constraints: an aspect ratio of 2 inside 300x300 gives 300x150; a size constraint caps it */
    i32 af = make(&w, sg, "Frame", "Aspect");
    set_ud2(&w, af, "Size", 0, 300, 0, 300);
    i32 ar = make(&w, af, "UIAspectRatioConstraint", "AR");
    set_num(&w, ar, "AspectRatio", 2);
    CHECK(near_rect(g, &w, af, 0, 0, 300, 150));
    i32 sc = make(&w, af, "UISizeConstraint", "SC");
    set_v2(&w, sc, "MaxSize", 100, 1e9f);
    CHECK(near_rect(g, &w, af, 0, 0, 100, 150));
    /* UIScale doubles it, about its anchor point */
    ub_world_destroy(&w, ar);
    ub_world_destroy(&w, sc);
    i32 us = make(&w, af, "UIScale", "S");
    set_num(&w, us, "Scale", 2);
    set_v2(&w, af, "AnchorPoint", 0.5f, 0.5f);
    set_ud2(&w, af, "Position", 0, 400, 0, 300);
    CHECK(near_rect(g, &w, af, 100, 0, 600, 600));
    ub_world_destroy(&w, af);

    /* AutomaticSize: a label grows to its text (size 20, "abcd" = 40 wide) */
    i32 auto_l = make(&w, sg, "TextLabel", "Auto");
    set_str(&w, auto_l, "Text", "abcd");
    set_num(&w, auto_l, "TextSize", 20);
    set_ud2(&w, auto_l, "Size", 0, 10, 0, 10);
    set_enum(&w, auto_l, "AutomaticSize", 3);
    CHECK(near_rect(g, &w, auto_l, 0, 0, 40, 20));
    CHECK(ub_gui_info_of(g, &w, auto_l, &in) && in.text_w == 40 && in.text_h == 20);
    ub_world_destroy(&w, auto_l);

    /* ScrollingFrame: children sit on the canvas; CanvasPosition moves them */
    i32 sf = make(&w, sg, "ScrollingFrame", "Scroll");
    set_ud2(&w, sf, "Size", 0, 100, 0, 100);
    set_ud2(&w, sf, "Position", 0, 0, 0, 300);
    set_ud2(&w, sf, "CanvasSize", 0, 0, 0, 500);
    i32 item = make(&w, sf, "Frame", "Item");
    set_ud2(&w, item, "Size", 1, 0, 0, 30);
    set_ud2(&w, item, "Position", 0, 0, 0, 200);
    CHECK(near_rect(g, &w, item, 0, 500, 100, 30));
    set_v2(&w, sf, "CanvasPosition", 0, 150);
    CHECK(near_rect(g, &w, item, 0, 350, 100, 30));
    CHECK(ub_gui_info_of(g, &w, sf, &in) && in.canvas_h == 500 && in.window_h == 100);
    /* out of range positions are pulled back */
    set_v2(&w, sf, "CanvasPosition", 0, 9999);
    ub_gui_update(g, &w, 800, 636, NULL, NULL, 0);
    const ub_value *cp = ub_world_prop_named(&w, sf, "CanvasPosition");
    CHECK(cp && cp->v.v2.y == 400);
    /* the wheel scrolls it */
    ub_gui_input inp;
    memset(&inp, 0, sizeof inp);
    inp.mouse_free = true;
    inp.mouse_x = 50;
    inp.mouse_y = 36 + 350;
    inp.wheel = 1;
    ub_gui_clear_events(g);
    ub_gui_update(g, &w, 800, 636, NULL, &inp, 0);
    cp = ub_world_prop_named(&w, sf, "CanvasPosition");
    CHECK(cp && cp->v.v2.y < 400 && ub_gui_wheel_taken(g));
    ub_world_destroy(&w, sf);

    /* ZIndex: Global sorts across the whole ScreenGui; Sibling keeps children over parents */
    i32 a = make(&w, sg, "Frame", "A"), b = make(&w, sg, "Frame", "B");
    set_ud2(&w, a, "Size", 0, 50, 0, 50);
    set_ud2(&w, a, "Position", 0, 600, 0, 100);
    set_ud2(&w, b, "Size", 0, 50, 0, 50);
    set_ud2(&w, b, "Position", 0, 600, 0, 100);
    i32 achild = make(&w, a, "Frame", "AChild");
    set_ud2(&w, achild, "Size", 1, 0, 1, 0);
    set_int(&w, a, "ZIndex", 2);
    set_int(&w, achild, "ZIndex", 1);
    i32 at[8];
    int nat = ub_gui_objects_at(g, &w, 610, 110, at, 8);
    CHECK(nat == 3 && at[0] == a); /* Global: A (2) over B (1) over A's child (1, but after B? no: before) */
    set_enum(&w, sg, "ZIndexBehavior", 1); /* Sibling */
    nat = ub_gui_objects_at(g, &w, 610, 110, at, 8);
    CHECK(nat == 3 && at[0] == achild && at[1] == a && at[2] == b);
    set_bool(&w, a, "Visible", false);
    nat = ub_gui_objects_at(g, &w, 610, 110, at, 8);
    CHECK(nat == 1 && at[0] == b);
    ub_world_destroy(&w, a);
    ub_world_destroy(&w, b);

    /* drawing: the panel's colour lands where it is on the screen */
    set_color(&w, f, "BackgroundColor3", 1, 0, 0);
    set_num(&w, f, "BackgroundTransparency", 0);
    ub_gui_render(g, &w, NULL);
    const int *rects;
    int nr;
    const ub_render_image *img = ub_gui_screen(g, &rects, &nr);
    CHECK(img && img->w == 800 && img->h == 636 && nr >= 1);
    if (img) {
        u32 c = img->px[(36 + 580) * img->pitch + 500]; /* the panel, clear of its labels */
        CHECK(c == 0xFFFF0000u);
        CHECK(img->px[(36 + 100) * img->pitch + 220] == 0);
    }
    ub_gui_free(g);
    ub_world_free(&w);
}

static void test_input(void)
{
    ub_world w;
    ub_world_init(&w);
    i32 pg = make(&w, -1, "PlayerGui", "PlayerGui");
    i32 sg = make(&w, pg, "ScreenGui", "Hud");
    i32 btn = make(&w, sg, "TextButton", "Button");
    set_ud2(&w, btn, "Size", 0, 100, 0, 40);
    set_ud2(&w, btn, "Position", 0, 10, 0, 10);
    i32 box = make(&w, sg, "TextBox", "Box");
    set_ud2(&w, box, "Size", 0, 200, 0, 30);
    set_ud2(&w, box, "Position", 0, 10, 0, 100);
    i32 cover = make(&w, sg, "Frame", "Cover"); /* not Active: clicks go through it */
    set_ud2(&w, cover, "Size", 0, 30, 0, 30);
    set_ud2(&w, cover, "Position", 0, 10, 0, 10);
    set_num(&w, cover, "BackgroundTransparency", 1);
    ub_gui *g = ub_gui_new();
    ub_gui_set_root(g, pg);
    ub_gui_input in;
    memset(&in, 0, sizeof in);
    in.mouse_free = true;
    in.mouse_x = 300;
    in.mouse_y = 300;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0);
    ub_gui_clear_events(g);
    /* onto the button (through the frame on top of it) */
    in.mouse_x = 20;
    in.mouse_y = 36 + 20;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.1);
    CHECK(count_events(g, btn, UB_GEV_MOUSE_ENTER) == 1 && ub_gui_mouse_taken(g));
    ub_gui_clear_events(g);
    in.down[0] = in.pressed[0] = true;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.2);
    CHECK(count_events(g, btn, UB_GEV_B1_DOWN) == 1 && count_events(g, btn, UB_GEV_INPUT_BEGAN) == 1);
    in.pressed[0] = in.down[0] = false;
    in.released[0] = true;
    ub_gui_clear_events(g);
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.3);
    CHECK(count_events(g, btn, UB_GEV_B1_UP) == 1 && count_events(g, btn, UB_GEV_B1_CLICK) == 1 &&
          count_events(g, btn, UB_GEV_ACTIVATED) == 1);
    in.released[0] = false;
    /* an Active frame over the button takes the click */
    set_bool(&w, cover, "Active", true);
    ub_gui_clear_events(g);
    in.pressed[0] = in.down[0] = true;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.4);
    in.pressed[0] = in.down[0] = false;
    in.released[0] = true;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.5);
    in.released[0] = false;
    CHECK(count_events(g, btn, UB_GEV_B1_CLICK) == 0 && ub_gui_mouse_taken(g));
    /* away: MouseLeave */
    ub_gui_clear_events(g);
    in.mouse_x = 600;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 0.6);
    CHECK(count_events(g, btn, UB_GEV_MOUSE_LEAVE) == 1 && !ub_gui_mouse_taken(g));

    /* the text box: click, type, Enter */
    set_str(&w, box, "Text", "old");
    in.mouse_x = 50;
    in.mouse_y = 36 + 110;
    in.pressed[0] = in.down[0] = true;
    ub_gui_clear_events(g);
    ub_gui_update(g, &w, 640, 480, NULL, &in, 1);
    in.pressed[0] = in.down[0] = false;
    CHECK(ub_gui_keyboard_taken(g) && ub_gui_focused_box(g) == box && count_events(g, box, UB_GEV_FOCUSED) == 1);
    const ub_value *t = ub_world_prop_named(&w, box, "Text");
    CHECK(t && t->v.str.len == 0); /* ClearTextOnFocus */
    in.text = "h\xC3\xA9llo\b\b";
    ub_gui_update(g, &w, 640, 480, NULL, &in, 1.1);
    t = ub_world_prop_named(&w, box, "Text");
    CHECK(t && !strcmp(t->v.str.s, "h\xC3\xA9l"));
    in.text = "!";
    in.key_pressed[UB_KEY_HOME] = true;
    ub_gui_update(g, &w, 640, 480, NULL, &in, 1.2);
    in.key_pressed[UB_KEY_HOME] = false;
    in.text = "\r";
    ub_gui_clear_events(g);
    ub_gui_update(g, &w, 640, 480, NULL, &in, 1.3);
    in.text = NULL;
    t = ub_world_prop_named(&w, box, "Text");
    CHECK(t && !strcmp(t->v.str.s, "h\xC3\xA9l!"));
    const ub_gui_event *ev;
    int n = ub_gui_events(g, &ev), lost = 0;
    for (int i = 0; i < n; ++i) lost += ev[i].inst == box && ev[i].type == UB_GEV_FOCUS_LOST && ev[i].flag == 1;
    CHECK(lost == 1 && !ub_gui_keyboard_taken(g));
    ub_gui_render(g, &w, NULL);
    ub_gui_free(g);
    ub_world_free(&w);
}

void test_gui(void)
{
    test_canvas();
    test_images();
    test_text();
    test_layout();
    test_input();
}
