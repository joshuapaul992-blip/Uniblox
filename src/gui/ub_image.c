/* Uniblox - PNG and BMP decoding and the image cache (see ub_image.h). */
#include "gui/ub_image.h"
#include "core/ub_compress.h"
#include "core/ub_log.h"
#include "core/ub_path.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 le32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 le16(const u8 *p) { return p[0] | ((u32)p[1] << 8); }

static u32 premul(u32 r, u32 g, u32 b, u32 a)
{
    if (a == 255) return 0xFF000000u | (r << 16) | (g << 8) | b;
    r = (r * a + 127) / 255;
    g = (g * a + 127) / 255;
    b = (b * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ----------------------------------------------------------------- PNG */

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undoes the filters of one (sub-)image: `rows` rows of `rowbytes` bytes,
 * each led by its filter byte. */
static bool unfilter(u8 *d, u32 rows, u32 rowbytes, u32 bpp)
{
    u8 *prev = NULL;
    for (u32 y = 0; y < rows; ++y) {
        u8 f = d[0], *row = d + 1;
        if (f > 4) return false;
        for (u32 i = 0; i < rowbytes; ++i) {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = row[i];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, c); break;
            default: break;
            }
            row[i] = (u8)v;
        }
        prev = row;
        d += rowbytes + 1;
    }
    return true;
}

typedef struct {
    u32 w, h;
    u8 depth, ctype, interlace;
    u32 pal[256];
    u32 npal;
    bool trns;
    u16 trns_g, trns_r, trns_gg, trns_b; /* grey or RGB key */
} pnginfo;

static u32 sample_bits(const u8 *row, u32 i, u8 depth)
{
    if (depth == 8) return row[i];
    if (depth == 16) return row[i * 2]; /* top byte */
    u32 bit = i * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

static u32 png_pixel(const pnginfo *in, const u8 *row, u32 x)
{
    u32 ch = in->ctype == 2 ? 3 : in->ctype == 4 ? 2 : in->ctype == 6 ? 4 : 1;
    u32 i = x * ch;
    u32 maxv = (1u << (in->depth > 8 ? 8 : in->depth)) - 1;
    switch (in->ctype) {
    case 0: {
        u32 v = sample_bits(row, i, in->depth);
        u32 raw = in->depth == 16 ? ((u32)row[i * 2] << 8) | row[i * 2 + 1] : v;
        u32 a = in->trns && raw == in->trns_g ? 0 : 255;
        v = v * 255 / maxv;
        return premul(v, v, v, a);
    }
    case 2: {
        u32 r = sample_bits(row, i, in->depth), g = sample_bits(row, i + 1, in->depth), b = sample_bits(row, i + 2, in->depth);
        u32 a = 255;
        if (in->trns) {
            u32 rr = in->depth == 16 ? ((u32)row[i * 2] << 8) | row[i * 2 + 1] : r;
            u32 gg = in->depth == 16 ? ((u32)row[i * 2 + 2] << 8) | row[i * 2 + 3] : g;
            u32 bb = in->depth == 16 ? ((u32)row[i * 2 + 4] << 8) | row[i * 2 + 5] : b;
            if (rr == in->trns_r && gg == in->trns_gg && bb == in->trns_b) a = 0;
        }
        return premul(r, g, b, a);
    }
    case 3: {
        u32 idx = sample_bits(row, i, in->depth);
        return idx < in->npal ? in->pal[idx] : 0;
    }
    case 4: {
        u32 v = sample_bits(row, i, in->depth), a = sample_bits(row, i + 1, in->depth);
        return premul(v, v, v, a);
    }
    default: {
        u32 r = sample_bits(row, i, in->depth), g = sample_bits(row, i + 1, in->depth);
        u32 b = sample_bits(row, i + 2, in->depth), a = sample_bits(row, i + 3, in->depth);
        return premul(r, g, b, a);
    }
    }
}

static bool png_decode(const u8 *d, u32 size, ub_image_px *out)
{
    pnginfo in;
    memset(&in, 0, sizeof in);
    u32 pos = 8, idat_len = 0, idat_cap = 0;
    u8 *idat = NULL;
    bool have_hdr = false, ok = false, ended = false;
    while (pos + 12 <= size && !ended) {
        u32 len = be32(d + pos);
        const u8 *type = d + pos + 4, *body = d + pos + 8;
        if (len > size - pos - 12) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            in.w = be32(body);
            in.h = be32(body + 4);
            in.depth = body[8];
            in.ctype = body[9];
            in.interlace = body[12];
            if (body[10] || body[11] || in.interlace > 1) break;
            bool depth_ok = (in.ctype == 0 && (in.depth == 1 || in.depth == 2 || in.depth == 4 || in.depth == 8 || in.depth == 16)) ||
                            (in.ctype == 3 && (in.depth == 1 || in.depth == 2 || in.depth == 4 || in.depth == 8)) ||
                            ((in.ctype == 2 || in.ctype == 4 || in.ctype == 6) && (in.depth == 8 || in.depth == 16));
            if (!depth_ok || in.w == 0 || in.h == 0 || in.w > UB_IMAGE_MAX_SIDE || in.h > UB_IMAGE_MAX_SIDE) break;
            have_hdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            in.npal = len / 3 > 256 ? 256 : len / 3;
            for (u32 i = 0; i < in.npal; ++i) in.pal[i] = premul(body[i * 3], body[i * 3 + 1], body[i * 3 + 2], 255);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (in.ctype == 3) {
                for (u32 i = 0; i < len && i < in.npal; ++i) {
                    u32 c = in.pal[i];
                    in.pal[i] = premul((c >> 16) & 255, (c >> 8) & 255, c & 255, body[i]);
                }
            } else if (in.ctype == 0 && len >= 2) {
                in.trns = true;
                in.trns_g = (u16)((body[0] << 8) | body[1]);
            } else if (in.ctype == 2 && len >= 6) {
                in.trns = true;
                in.trns_r = (u16)((body[0] << 8) | body[1]);
                in.trns_gg = (u16)((body[2] << 8) | body[3]);
                in.trns_b = (u16)((body[4] << 8) | body[5]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + len > idat_cap) {
                u32 nc = idat_cap ? idat_cap * 2 : 65536;
                while (nc < idat_len + len) nc *= 2;
                if (nc > 64u << 20) break;
                u8 *np = (u8 *)plat_alloc(nc);
                if (!np) break;
                if (idat) {
                    memcpy(np, idat, idat_len);
                    plat_free(idat);
                }
                idat = np;
                idat_cap = nc;
            }
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            ended = true;
        }
        pos += len + 12;
    }
    if (have_hdr && idat_len) {
        u32 ch = in.ctype == 2 ? 3 : in.ctype == 4 ? 2 : in.ctype == 6 ? 4 : 1;
        u32 bits = ch * in.depth, bpp = (bits + 7) / 8;
        /* the raw size: for Adam7, the seven passes' rows together */
        static const u8 ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const u8 adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
        int passes = in.interlace ? 7 : 1;
        u32 pw[7], ph[7];
        u64 raw = 0;
        for (int p = 0; p < passes; ++p) {
            pw[p] = in.interlace ? (in.w + adx[p] - 1 - ax0[p]) / adx[p] : in.w;
            ph[p] = in.interlace ? (in.h + ady[p] - 1 - ay0[p]) / ady[p] : in.h;
            if (in.w <= ax0[p]) pw[p] = 0;
            if (in.h <= ay0[p]) ph[p] = 0;
            if (pw[p] && ph[p]) raw += (u64)ph[p] * (((u64)pw[p] * bits + 7) / 8 + 1);
        }
        u8 *buf = raw < (256u << 20) ? (u8 *)plat_alloc((size_t)raw + 1) : NULL;
        u32 *px = (u32 *)plat_alloc(sizeof(u32) * in.w * in.h);
        if (buf && px && ub_zlib_decompress(idat, idat_len, buf, (u32)raw) == (i32)raw) {
            ok = true;
            u8 *q = buf;
            for (int p = 0; p < passes && ok; ++p) {
                if (!pw[p] || !ph[p]) continue;
                u32 rowbytes = (pw[p] * bits + 7) / 8;
                if (!unfilter(q, ph[p], rowbytes, bpp)) {
                    ok = false;
                    break;
                }
                for (u32 y = 0; y < ph[p]; ++y) {
                    const u8 *row = q + y * (rowbytes + 1) + 1;
                    u32 oy = in.interlace ? ay0[p] + y * ady[p] : y;
                    for (u32 x = 0; x < pw[p]; ++x) {
                        u32 ox = in.interlace ? ax0[p] + x * adx[p] : x;
                        px[oy * in.w + ox] = png_pixel(&in, row, x);
                    }
                }
                q += ph[p] * (rowbytes + 1);
            }
        }
        plat_free(buf);
        if (ok) {
            out->px = px;
            out->w = (int)in.w;
            out->h = (int)in.h;
        } else {
            plat_free(px);
        }
    }
    plat_free(idat);
    return ok;
}

/* ----------------------------------------------------------------- BMP */

static bool bmp_decode(const u8 *d, u32 size, ub_image_px *out)
{
    if (size < 54) return false;
    u32 off = le32(d + 10), hs = le32(d + 14);
    if (hs < 40 || 14 + hs > size) return false;
    i32 w = (i32)le32(d + 18), h = (i32)le32(d + 22);
    u32 bpp = le16(d + 28), comp = le32(d + 30), ncol = le32(d + 46);
    bool top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > UB_IMAGE_MAX_SIDE || h > UB_IMAGE_MAX_SIDE) return false;
    if (!(comp == 0 || (comp == 3 && bpp == 32)) || (bpp != 8 && bpp != 24 && bpp != 32)) return false;
    u32 stride = (((u32)w * bpp + 31) / 32) * 4;
    if (off > size || (u64)stride * (u32)h > size - off) return false;
    u32 pal[256];
    if (bpp == 8) {
        if (!ncol || ncol > 256) ncol = 256;
        if (14 + hs + ncol * 4 > size) return false;
        for (u32 i = 0; i < ncol; ++i) {
            const u8 *e = d + 14 + hs + i * 4;
            pal[i] = 0xFF000000u | ((u32)e[2] << 16) | ((u32)e[1] << 8) | e[0];
        }
        for (u32 i = ncol; i < 256; ++i) pal[i] = 0xFF000000u;
    }
    /* 32-bit: use the alpha byte only if some pixel has it set */
    bool alpha = false;
    if (bpp == 32)
        for (i32 y = 0; y < h && !alpha; ++y)
            for (i32 x = 0; x < w && !alpha; ++x) alpha = d[off + (u32)y * stride + (u32)x * 4 + 3] != 0;
    u32 *px = (u32 *)plat_alloc(sizeof(u32) * (size_t)w * (size_t)h);
    if (!px) return false;
    for (i32 y = 0; y < h; ++y) {
        const u8 *row = d + off + (u32)(top_down ? y : h - 1 - y) * stride;
        for (i32 x = 0; x < w; ++x) {
            u32 c;
            if (bpp == 8) c = pal[row[x]];
            else if (bpp == 24) c = premul(row[x * 3 + 2], row[x * 3 + 1], row[x * 3], 255);
            else c = premul(row[x * 4 + 2], row[x * 4 + 1], row[x * 4], alpha ? row[x * 4 + 3] : 255);
            px[(size_t)y * w + x] = c;
        }
    }
    out->px = px;
    out->w = w;
    out->h = h;
    return true;
}

bool ub_image_decode(const u8 *data, u32 size, ub_image_px *out)
{
    memset(out, 0, sizeof *out);
    static const u8 png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (size >= 8 && !memcmp(data, png_sig, 8)) return png_decode(data, size, out);
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return bmp_decode(data, size, out);
    return false;
}

/* --------------------------------------------------------------- cache */

static bool starts(const char *s, const char *p)
{
    while (*p) {
        if ((*s | 32) != (*p | 32)) return false;
        ++s;
        ++p;
    }
    return true;
}

bool ub_image_path(const char *content, char *path, int cap, const char *ext)
{
    return ub_content_path(content, path, cap, ext);
}

#define IMG_SLOTS 256
#define IMG_BUDGET (96u << 20)
typedef struct {
    char key[200];
    ub_image_px img;
    bool loaded, missing;
    u32 used;      /* for evicting the least recently used */
} islot;
static islot *g_img;
static u32 g_img_tick, g_img_bytes;

void ub_image_flush(void)
{
    if (!g_img) return;
    for (int i = 0; i < IMG_SLOTS; ++i) plat_free((void *)g_img[i].img.px);
    plat_free(g_img);
    g_img = NULL;
    g_img_bytes = 0;
}

static bool load_file(const char *path, ub_image_px *out)
{
    void *data = NULL;
    u32 size = 0;
    if (!ub_path_is_safe(path) || !plat_file_exists(path) || !plat_file_read_all(path, 32u << 20, &data, &size))
        return false;
    bool ok = ub_image_decode((const u8 *)data, size, out);
    plat_free(data);
    return ok;
}

const ub_image_px *ub_image_get(const char *content)
{
    if (!content || !content[0]) return NULL;
    if (!g_img) {
        g_img = (islot *)plat_alloc(sizeof(islot) * IMG_SLOTS);
        if (!g_img) return NULL;
        memset(g_img, 0, sizeof(islot) * IMG_SLOTS);
    }
    g_img_tick++;
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < IMG_SLOTS; ++i) {
        islot *s = &g_img[i];
        if (s->loaded && !ub_strcmp(s->key, content)) {
            s->used = g_img_tick;
            return s->missing ? NULL : &s->img;
        }
        if (!s->loaded && free_slot < 0) free_slot = i;
        if (s->used < g_img[oldest].used) oldest = i;
    }
    if (free_slot < 0) { /* full: drop the least recently used */
        free_slot = oldest;
        g_img_bytes -= (u32)(g_img[oldest].img.w * g_img[oldest].img.h * 4);
        plat_free((void *)g_img[oldest].img.px);
        memset(&g_img[oldest], 0, sizeof(islot));
    }
    islot *s = &g_img[free_slot];
    memset(s, 0, sizeof *s);
    ub_fmt(s->key, sizeof s->key, "%s", content);
    s->loaded = true;
    s->used = g_img_tick;
    char path[300];
    static const char *const exts[] = { "png", "bmp" };
    bool found = false;
    for (int e = 0; e < 2 && !found; ++e) {
        if (!ub_image_path(content, path, sizeof path, exts[e])) break;
        found = load_file(path, &s->img);
        if (found) break;
        if (starts(content, "rbxasset://")) break; /* the path has its own extension */
    }
    u32 bytes = found ? (u32)(s->img.w * s->img.h * 4) : 0;
    if (found && g_img_bytes + bytes > IMG_BUDGET) { /* over budget: keep it out */
        plat_free((void *)s->img.px);
        memset(&s->img, 0, sizeof s->img);
        found = false;
    }
    if (!found) {
        s->missing = true;
        if (ub_image_path(content, path, sizeof path, "png"))
            ub_info("gui: no picture for %s (Uniblox never downloads; put your own in %s)", content, path);
        else
            ub_info("gui: no picture for %s", content);
        return NULL;
    }
    g_img_bytes += bytes;
    return &s->img;
}
