/* Uniblox - GUI text (see ub_text.h). */
#include "gui/ub_text.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

/* -------------------------------------------------------------- faces */

static ub_typeface face(u8 family, u8 weight, u8 italic)
{
    ub_typeface f;
    f.family = family;
    f.weight = weight;
    f.italic = italic;
    f.pad = 0;
    return f;
}

ub_typeface ub_text_face_enum(int font)
{
    /* Enum.Font, 2022: Uniblox's nearest system typeface for each */
    static const u8 k[][3] = {
        { UB_FACE_SANS, 4, 0 },  /* Legacy */           { UB_FACE_SANS, 4, 0 },  /* Arial */
        { UB_FACE_SANS, 7, 0 },  /* ArialBold */        { UB_FACE_SANS, 4, 0 },  /* SourceSans */
        { UB_FACE_SANS, 7, 0 },  /* SourceSansBold */   { UB_FACE_SANS, 3, 0 },  /* SourceSansLight */
        { UB_FACE_SANS, 4, 1 },  /* SourceSansItalic */ { UB_FACE_SERIF, 4, 0 }, /* Bodoni */
        { UB_FACE_SERIF, 4, 0 }, /* Garamond */         { UB_FACE_HAND, 4, 0 },  /* Cartoon */
        { UB_FACE_MONO, 4, 0 },  /* Code */             { UB_FACE_SANS, 4, 0 },  /* Highway */
        { UB_FACE_SANS, 4, 0 },  /* SciFi */            { UB_FACE_MONO, 7, 0 },  /* Arcade */
        { UB_FACE_SERIF, 4, 0 }, /* Fantasy */          { UB_FACE_SERIF, 4, 0 }, /* Antique */
        { UB_FACE_SANS, 6, 0 },  /* SourceSansSemibold */ { UB_FACE_SANS, 4, 0 }, /* Gotham */
        { UB_FACE_SANS, 5, 0 },  /* GothamMedium */     { UB_FACE_SANS, 7, 0 },  /* GothamBold */
        { UB_FACE_SANS, 9, 0 },  /* GothamBlack */      { UB_FACE_HAND, 4, 0 },  /* AmaticSC */
        { UB_FACE_HAND, 7, 0 },  /* Bangers */          { UB_FACE_HAND, 4, 0 },  /* Creepster */
        { UB_FACE_SANS, 7, 0 },  /* DenkOne */          { UB_FACE_SERIF, 4, 0 }, /* Fondamento */
        { UB_FACE_SANS, 7, 0 },  /* FredokaOne */       { UB_FACE_SERIF, 7, 0 }, /* GrenzeGotisch */
        { UB_FACE_HAND, 4, 0 },  /* IndieFlower */      { UB_FACE_SANS, 4, 0 },  /* JosefinSans */
        { UB_FACE_SANS, 4, 0 },  /* Jura */             { UB_FACE_HAND, 4, 0 },  /* Kalam */
        { UB_FACE_HAND, 9, 0 },  /* LuckiestGuy */      { UB_FACE_SERIF, 4, 0 }, /* Merriweather */
        { UB_FACE_SANS, 4, 0 },  /* Michroma */         { UB_FACE_SANS, 4, 0 },  /* Nunito */
        { UB_FACE_SANS, 5, 0 },  /* Oswald */           { UB_FACE_HAND, 4, 0 },  /* PatrickHand */
        { UB_FACE_HAND, 7, 0 },  /* PermanentMarker */  { UB_FACE_SANS, 4, 0 },  /* Roboto */
        { UB_FACE_SANS, 4, 0 },  /* RobotoCondensed */  { UB_FACE_MONO, 4, 0 },  /* RobotoMono */
        { UB_FACE_SANS, 4, 0 },  /* Sarpanch */         { UB_FACE_MONO, 4, 0 },  /* SpecialElite */
        { UB_FACE_SANS, 4, 0 },  /* TitilliumWeb */     { UB_FACE_SANS, 4, 0 },  /* Ubuntu */
    };
    if (font < 0 || font >= (int)(sizeof k / sizeof k[0])) return face(UB_FACE_SANS, 4, 0);
    return face(k[font][0], k[font][1], k[font][2]);
}

static bool has(const char *s, const char *needle)
{
    for (; *s; ++s) {
        const char *a = s, *b = needle;
        while (*a && *b && (*a | 32) == (*b | 32)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

ub_typeface ub_text_face_family(const char *family, int weight, int style)
{
    u8 fam = UB_FACE_SANS;
    static const char *const mono[] = { "Mono", "Code", "Inconsolata", "SpecialElite", "Arcade", "Courier", NULL };
    static const char *const serif[] = { "Bodoni", "Garamond", "Merriweather", "Antique", "Fondamento", "Grenze",
                                         "Fantasy", "Serif", "Times", "Accanthis", "Guru", NULL };
    static const char *const hand[] = { "Comic", "Cartoon", "Indie", "Patrick", "Kalam", "Permanent", "Amatic",
                                        "Bangers", "Creepster", "Luckiest", NULL };
    for (int i = 0; mono[i]; ++i) if (has(family, mono[i])) fam = UB_FACE_MONO;
    for (int i = 0; serif[i]; ++i) if (has(family, serif[i]) && !has(family, "Sans")) fam = UB_FACE_SERIF;
    for (int i = 0; hand[i]; ++i) if (has(family, hand[i])) fam = UB_FACE_HAND;
    int w = (weight + 50) / 100;
    return face(fam, (u8)UB_CLAMP(w, 1, 9), style ? 1 : 0);
}

static u32 face_code(ub_typeface f) { return (u32)f.family | ((u32)f.weight << 8) | ((u32)f.italic << 16); }

/* ------------------------------------------------------------- caches */

#define MET_SLOTS (UB_FACE_FAMILIES * 10 * 2)
static struct { bool ok; f32 asc, desc; } g_met[MET_SLOTS];

static void metrics(ub_typeface f, f32 *asc, f32 *desc)
{
    int i = ((int)f.family % UB_FACE_FAMILIES) * 20 + (f.weight % 10) * 2 + (f.italic ? 1 : 0);
    if (!g_met[i].ok) {
        f32 a = 0.8f, d = 0.2f;
        if (!plat_face_metrics(f, &a, &d) || !(a > 0.1f) || !(d >= 0) || a + d > 4) {
            a = 0.8f;
            d = 0.2f;
        }
        g_met[i].asc = a;
        g_met[i].desc = d;
        g_met[i].ok = true;
    }
    *asc = g_met[i].asc;
    *desc = g_met[i].desc;
}

/* pixels per em for a TextSize: the line (ascent + descent) is TextSize */
static f32 em_for(ub_typeface f, f32 size)
{
    f32 a, d;
    metrics(f, &a, &d);
    return size / (a + d);
}

#define ADV_SLOTS 8192
static struct { u32 face, cp; f32 adv; } g_adv[ADV_SLOTS];
static int g_adv_used;

static f32 advance_em(ub_typeface f, u32 cp)
{
    u32 fc = face_code(f) + 1; /* 0 = empty slot */
    u32 h = (cp * 2654435761u ^ fc * 40503u) & (ADV_SLOTS - 1);
    for (int probe = 0; probe < 16; ++probe, h = (h + 1) & (ADV_SLOTS - 1)) {
        if (g_adv[h].face == fc && g_adv[h].cp == cp) return g_adv[h].adv;
        if (!g_adv[h].face) break;
    }
    f32 a = plat_glyph_advance(f, cp);
    if (!(a >= 0) || a > 4) a = 0.5f;
    if (g_adv_used > ADV_SLOTS * 3 / 4) { /* full: start again */
        memset(g_adv, 0, sizeof g_adv);
        g_adv_used = 0;
    }
    h = (cp * 2654435761u ^ fc * 40503u) & (ADV_SLOTS - 1);
    while (g_adv[h].face) h = (h + 1) & (ADV_SLOTS - 1);
    g_adv[h].face = fc;
    g_adv[h].cp = cp;
    g_adv[h].adv = a;
    g_adv_used++;
    return a;
}

#define GLYPH_SLOTS 4096
#define GLYPH_POOL (3u << 20)
typedef struct {
    u32 face, cp;
    i32 em4;              /* pixels per em x 4 */
    i32 x0, y0, w, h;
    u32 off;
    bool used;
} gslot;
static gslot g_gl[GLYPH_SLOTS];
static u8 *g_pool;
static u32 g_pool_len;
static int g_gl_used;

void ub_text_flush(void)
{
    memset(g_gl, 0, sizeof g_gl);
    g_gl_used = 0;
    g_pool_len = 0;
    memset(g_adv, 0, sizeof g_adv);
    g_adv_used = 0;
    memset(g_met, 0, sizeof g_met);
}

static const gslot *glyph(ub_typeface f, u32 cp, f32 em_px)
{
    if (!g_pool) {
        g_pool = (u8 *)plat_alloc(GLYPH_POOL);
        if (!g_pool) return NULL;
    }
    u32 fc = face_code(f) + 1;
    i32 em4 = (i32)(em_px * 4.0f + 0.5f);
    if (em4 < 4) em4 = 4;
    if (em4 > 4 * 400) em4 = 4 * 400;
    u32 h = (cp * 2654435761u ^ fc * 40503u ^ (u32)em4 * 2246822519u) & (GLYPH_SLOTS - 1);
    for (int probe = 0; probe < 32; ++probe, h = (h + 1) & (GLYPH_SLOTS - 1)) {
        if (!g_gl[h].used) break;
        if (g_gl[h].face == fc && g_gl[h].cp == cp && g_gl[h].em4 == em4) return &g_gl[h];
    }
    /* draw it: room for a glyph up to 3 ems square */
    f32 em = (f32)em4 * 0.25f;
    u32 need = (u32)(em * 3 + 8) * (u32)(em * 3 + 8);
    if (g_gl_used > GLYPH_SLOTS * 3 / 4 || g_pool_len + need > GLYPH_POOL) {
        memset(g_gl, 0, sizeof g_gl);
        g_gl_used = 0;
        g_pool_len = 0;
    }
    if (need > GLYPH_POOL) return NULL;
    int x0, y0, w, gh;
    if (!plat_glyph(f, cp, em, g_pool + g_pool_len, (int)need, &x0, &y0, &w, &gh)) w = gh = x0 = y0 = 0;
    h = (cp * 2654435761u ^ fc * 40503u ^ (u32)em4 * 2246822519u) & (GLYPH_SLOTS - 1);
    while (g_gl[h].used) h = (h + 1) & (GLYPH_SLOTS - 1);
    gslot *s = &g_gl[h];
    s->used = true;
    s->face = fc;
    s->cp = cp;
    s->em4 = em4;
    s->x0 = x0;
    s->y0 = y0;
    s->w = w;
    s->h = gh;
    s->off = g_pool_len;
    g_pool_len += (u32)(w * gh);
    g_gl_used++;
    return s;
}

/* ------------------------------------------------------------ building */

void ub_text_layout_init(ub_text_layout *l) { memset(l, 0, sizeof *l); }

void ub_text_layout_free(ub_text_layout *l)
{
    plat_free(l->g);
    plat_free(l->runs);
    memset(l, 0, sizeof *l);
}

static bool push_glyph(ub_text_layout *l, u32 cp, u16 run)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        if (nc > 1 << 20) return false;
        ub_tglyph *ng = (ub_tglyph *)plat_alloc(sizeof(ub_tglyph) * (size_t)nc);
        if (!ng) return false;
        if (l->g) {
            memcpy(ng, l->g, sizeof(ub_tglyph) * (size_t)l->n);
            plat_free(l->g);
        }
        l->g = ng;
        l->cap = nc;
    }
    ub_tglyph *g = &l->g[l->n++];
    memset(g, 0, sizeof *g);
    g->cp = cp;
    g->run = run;
    return true;
}

static int push_run(ub_text_layout *l, const ub_trun *r)
{
    if (l->nruns && memcmp(&l->runs[l->nruns - 1], r, sizeof *r) == 0) return l->nruns - 1;
    if (l->nruns == l->runs_cap) {
        int nc = l->runs_cap ? l->runs_cap * 2 : 8;
        if (nc > 60000) return l->nruns - 1;
        ub_trun *nr = (ub_trun *)plat_alloc(sizeof(ub_trun) * (size_t)nc);
        if (!nr) return l->nruns - 1;
        if (l->runs) {
            memcpy(nr, l->runs, sizeof(ub_trun) * (size_t)l->nruns);
            plat_free(l->runs);
        }
        l->runs = nr;
        l->runs_cap = nc;
    }
    l->runs[l->nruns] = *r;
    return l->nruns++;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* color="#RRGGBB" or "rgb(r,g,b)" */
static bool parse_color(const char *v, int n, u32 *out)
{
    if (n == 7 && v[0] == '#') {
        u32 c = 0;
        for (int i = 1; i < 7; ++i) {
            int h = hexv(v[i]);
            if (h < 0) return false;
            c = c * 16 + (u32)h;
        }
        *out = c;
        return true;
    }
    if (n > 5 && (v[0] | 32) == 'r' && (v[1] | 32) == 'g' && (v[2] | 32) == 'b' && v[3] == '(') {
        int comp[3] = { 0, 0, 0 }, k = 0;
        for (int i = 4; i < n && k < 3; ++i) {
            if (v[i] >= '0' && v[i] <= '9') comp[k] = comp[k] * 10 + (v[i] - '0');
            else if (v[i] == ',') ++k;
            else if (v[i] == ')') break;
        }
        *out = ((u32)UB_MIN(comp[0], 255) << 16) | ((u32)UB_MIN(comp[1], 255) << 8) | (u32)UB_MIN(comp[2], 255);
        return true;
    }
    return false;
}

static f32 parse_num(const char *v, int n)
{
    f32 x = 0, scale = 0;
    bool neg = false;
    for (int i = 0; i < n; ++i) {
        if (v[i] == '-' && i == 0) neg = true;
        else if (v[i] == '.') scale = 1;
        else if (v[i] >= '0' && v[i] <= '9') {
            if (scale > 0) {
                scale *= 0.1f;
                x += (f32)(v[i] - '0') * scale;
            } else {
                x = x * 10 + (f32)(v[i] - '0');
            }
        }
    }
    return neg ? -x : x;
}

/* An attribute of a tag: name="value" (or 'value'); false if absent. */
static bool attr(const char *tag, int n, const char *name, const char **val, int *vn)
{
    int nl = (int)ub_strlen(name);
    for (int i = 0; i + nl < n; ++i) {
        bool m = true;
        for (int k = 0; k < nl && m; ++k) m = (tag[i + k] | 32) == (name[k] | 32);
        if (!m || (i > 0 && tag[i - 1] != ' ')) continue;
        int j = i + nl;
        while (j < n && tag[j] == ' ') ++j;
        if (j >= n || tag[j] != '=') continue;
        ++j;
        while (j < n && tag[j] == ' ') ++j;
        if (j >= n || (tag[j] != '"' && tag[j] != '\'')) continue;
        char q = tag[j++];
        int s = j;
        while (j < n && tag[j] != q) ++j;
        *val = tag + s;
        *vn = j - s;
        return true;
    }
    return false;
}

static bool tag_is(const char *t, int n, const char *name)
{
    int nl = (int)ub_strlen(name);
    if (n < nl) return false;
    for (int i = 0; i < nl; ++i)
        if ((t[i] | 32) != name[i]) return false;
    return n == nl || t[nl] == ' ' || t[nl] == '/';
}

#define RICH_DEPTH 32

typedef struct {
    ub_trun run;
    u8 upper, smallcaps;
    char name[12];
} rstate;

/* Text -> glyphs with their runs. Rich text tags change the run;
 * malformed markup is shown as written. */
static bool build_glyphs(const ub_text_style *s, ub_text_layout *l)
{
    rstate st[RICH_DEPTH];
    int depth = 0;
    memset(&st[0], 0, sizeof st[0]);
    st[0].run = s->base;
    int run = push_run(l, &st[0].run);
    const char *p = s->text, *end = s->text + s->len;
    i32 grapheme = 0;
    while (p < end) {
        if (s->rich && *p == '<') {
            const char *q = p + 1;
            if (end - p >= 4 && q[0] == '!' && q[1] == '-' && q[2] == '-') { /* <!-- comment --> */
                const char *e = q + 3;
                while (e + 2 < end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) ++e;
                if (e + 2 < end) {
                    p = e + 3;
                    continue;
                }
            }
            const char *close = q;
            while (close < end && *close != '>' && *close != '<') ++close;
            if (close < end && *close == '>') {
                const char *t = q;
                int tn = (int)(close - q);
                bool closing = tn > 0 && t[0] == '/';
                if (closing) { ++t; --tn; }
                while (tn > 0 && t[tn - 1] == ' ') --tn;
                bool selfclose = tn > 0 && t[tn - 1] == '/';
                if (selfclose) --tn;
                bool known = true;
                if (tag_is(t, tn, "br")) {
                    if (!push_glyph(l, '\n', (u16)run)) return false;
                } else if (closing) {
                    if (depth > 0) {
                        --depth;
                        run = push_run(l, &st[depth].run);
                    }
                } else if (depth + 1 < RICH_DEPTH) {
                    rstate n = st[depth];
                    const char *v;
                    int vn;
                    if (tag_is(t, tn, "b")) n.run.face.weight = 7;
                    else if (tag_is(t, tn, "i")) n.run.face.italic = 1;
                    else if (tag_is(t, tn, "u")) n.run.underline = 1;
                    else if (tag_is(t, tn, "s")) n.run.strike = 1;
                    else if (tag_is(t, tn, "uppercase") || tag_is(t, tn, "uc")) n.upper = 1;
                    else if (tag_is(t, tn, "smallcaps") || tag_is(t, tn, "sc")) n.smallcaps = 1;
                    else if (tag_is(t, tn, "font")) {
                        u32 col;
                        if (attr(t, tn, "color", &v, &vn) && parse_color(v, vn, &col)) n.run.color = col;
                        if (attr(t, tn, "size", &v, &vn)) n.run.size = ub_clampf(parse_num(v, vn), 1, 100);
                        if (attr(t, tn, "transparency", &v, &vn)) n.run.transparency = ub_clampf(parse_num(v, vn), 0, 1);
                        if (attr(t, tn, "weight", &v, &vn)) {
                            f32 wnum = parse_num(v, vn);
                            if (wnum >= 100) n.run.face.weight = (u8)UB_CLAMP((int)(wnum + 50) / 100, 1, 9);
                            else if (vn >= 4 && (v[0] | 32) == 'b') n.run.face.weight = 7;
                            else if (vn >= 4 && (v[0] | 32) == 'h') n.run.face.weight = 9;
                            else if (vn >= 4 && (v[0] | 32) == 'l') n.run.face.weight = 3;
                            else if (vn >= 4 && (v[0] | 32) == 'r') n.run.face.weight = 4;
                        }
                        if (attr(t, tn, "face", &v, &vn) || attr(t, tn, "family", &v, &vn)) {
                            char fam[96];
                            int k = UB_MIN(vn, 95);
                            memcpy(fam, v, (size_t)k);
                            fam[k] = 0;
                            ub_typeface nf = ub_text_face_family(fam, n.run.face.weight * 100, n.run.face.italic);
                            n.run.face.family = nf.family;
                        }
                    } else if (tag_is(t, tn, "stroke")) {
                        u32 col = 0;
                        n.run.stroke = 1;
                        n.run.stroke_color = 0;
                        n.run.stroke_thickness = 1;
                        n.run.stroke_transparency = 0;
                        if (attr(t, tn, "color", &v, &vn) && parse_color(v, vn, &col)) n.run.stroke_color = col;
                        if (attr(t, tn, "thickness", &v, &vn)) n.run.stroke_thickness = ub_clampf(parse_num(v, vn), 0, 20);
                        if (attr(t, tn, "transparency", &v, &vn))
                            n.run.stroke_transparency = ub_clampf(parse_num(v, vn), 0, 1);
                    } else if (tag_is(t, tn, "mark")) {
                        /* highlighting: not drawn yet, but the tag is understood */
                    } else {
                        known = false;
                    }
                    if (known && !selfclose) {
                        st[++depth] = n;
                        run = push_run(l, &n.run);
                    }
                }
                if (known) {
                    p = close + 1;
                    continue;
                }
            }
        }
        u32 cp;
        if (s->rich && *p == '&') { /* entities */
            static const struct { const char *e; u32 cp; } ents[] = {
                { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", 0xA0 },
            };
            bool hit = false;
            for (int i = 0; i < (int)(sizeof ents / sizeof ents[0]) && !hit; ++i) {
                int el = (int)ub_strlen(ents[i].e);
                if (end - p >= el && memcmp(p, ents[i].e, (size_t)el) == 0) {
                    if (!push_glyph(l, ents[i].cp, (u16)run)) return false;
                    l->g[l->n - 1].index = grapheme++;
                    p += el;
                    hit = true;
                }
            }
            if (hit) continue;
        }
        const char *pp = p;
        cp = ub_utf8_decode(&pp);
        if (pp <= p) pp = p + 1;
        p = pp;
        if (cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        if (st[depth].upper && cp >= 'a' && cp <= 'z') cp -= 32;
        int use_run = run;
        if (st[depth].smallcaps && cp >= 'a' && cp <= 'z') {
            ub_trun small = st[depth].run;
            small.size *= 0.8f;
            use_run = push_run(l, &small);
            cp -= 32;
        }
        if (!push_glyph(l, cp, (u16)use_run)) return false;
        l->g[l->n - 1].index = cp == '\n' ? -1 : grapheme++;
        if (use_run != run) run = push_run(l, &st[depth].run);
    }
    l->graphemes = grapheme;
    return true;
}

/* -------------------------------------------------------------- layout */

static f32 glyph_adv(const ub_text_layout *l, const ub_tglyph *g)
{
    const ub_trun *r = &l->runs[g->run];
    if (g->cp == '\n') return 0;
    return advance_em(r->face, g->cp) * em_for(r->face, r->size);
}

typedef struct {
    int first, last;     /* glyphs [first, last) */
    f32 width;
    f32 size;            /* largest run size on the line */
} tline;

static void place_lines(const ub_text_style *s, ub_text_layout *l, tline *lines, int nl)
{
    f32 total = 0, widest = 0;
    for (int i = 0; i < nl; ++i) {
        total += lines[i].size * s->line_height;
        if (lines[i].width > widest) widest = lines[i].width;
    }
    l->bounds_w = widest;
    l->bounds_h = total;
    l->lines = nl;
    f32 y = s->yalign == 0 ? 0 : s->yalign == 2 ? s->box_h - total : (s->box_h - total) * 0.5f;
    for (int i = 0; i < nl; ++i) {
        tline *ln = &lines[i];
        f32 lh = ln->size * s->line_height;
        /* the baseline sits like a single line of this size, centred in the line height */
        const ub_trun *r0 = &l->runs[ln->first < l->n ? l->g[ln->first].run : 0];
        f32 a, d;
        metrics(r0->face, &a, &d);
        f32 base = y + (lh - ln->size) * 0.5f + ln->size * a / (a + d);
        f32 x = s->xalign == 0 ? 0 : s->xalign == 1 ? s->box_w - ln->width : (s->box_w - ln->width) * 0.5f;
        for (int k = ln->first; k < ln->last; ++k) {
            l->g[k].x = x;
            l->g[k].y = base;
            x += l->g[k].adv;
        }
        y += lh;
    }
}

bool ub_text_lay_out(const ub_text_style *s, ub_text_layout *l)
{
    l->n = 0;
    l->nruns = 0;
    l->bounds_w = l->bounds_h = 0;
    l->lines = 0;
    l->fits = true;
    l->broke_word = false;
    if (!build_glyphs(s, l)) return false;
    for (int i = 0; i < l->n; ++i) l->g[i].adv = glyph_adv(l, &l->g[i]);

    /* line breaking */
    int cap = 16, nl = 0;
    tline *lines = (tline *)plat_alloc(sizeof(tline) * (size_t)cap);
    if (!lines) return false;
    int start = 0;
    while (start <= l->n) {
        f32 x = 0, size = 0;
        int i = start, brk = -1;
        for (; i < l->n; ++i) {
            const ub_tglyph *g = &l->g[i];
            if (g->cp == '\n') break;
            f32 rs = l->runs[g->run].size;
            if (s->wrap && x + g->adv > s->box_w + 0.01f && i > start && g->cp != ' ') {
                if (brk > start) i = brk; /* back to after the last space */
                else l->broke_word = true;
                break;
            }
            if (g->cp == ' ') brk = i + 1;
            if (rs > size) size = rs;
            x += g->adv;
        }
        int last = i;
        /* measured width without trailing spaces */
        f32 w = 0;
        for (int k = start; k < last; ++k) w += l->g[k].adv;
        for (int k = last - 1; k >= start && l->g[k].cp == ' '; --k) w -= l->g[k].adv;
        if (size == 0) size = l->runs[l->n > 0 && start < l->n ? l->g[start].run : 0].size;
        if (nl == cap) {
            tline *n2 = (tline *)plat_alloc(sizeof(tline) * (size_t)cap * 2);
            if (!n2) { plat_free(lines); return false; }
            memcpy(n2, lines, sizeof(tline) * (size_t)cap);
            plat_free(lines);
            lines = n2;
            cap *= 2;
        }
        lines[nl].first = start;
        lines[nl].last = last;
        lines[nl].width = w > 0 ? w : 0;
        lines[nl].size = size;
        nl++;
        if (i >= l->n) break;
        start = l->g[i].cp == '\n' ? i + 1 : i;
        /* a wrapped line drops the spaces it broke at */
        if (l->g[i].cp != '\n') while (start < l->n && l->g[start].cp == ' ') ++start;
        if (start >= l->n && l->g[i].cp != '\n') break;
    }
    if (l->n == 0) {
        nl = 1;
        lines[0].first = lines[0].last = 0;
        lines[0].width = 0;
        lines[0].size = s->base.size;
    }
    place_lines(s, l, lines, nl);
    l->fits = l->bounds_w <= s->box_w + 0.5f && l->bounds_h <= s->box_h + 0.5f;

    /* TextTruncate.AtEnd: drop what does not fit and end with "..." */
    if (s->truncate && !l->fits && l->n > 0) {
        f32 ell = 3 * advance_em(s->base.face, '.') * em_for(s->base.face, s->base.size);
        /* the last line that fits in height */
        int keep_lines = 0;
        f32 hsum = 0;
        for (int i = 0; i < nl; ++i) {
            f32 lh = lines[i].size * s->line_height;
            if (hsum + lh > s->box_h + 0.5f && keep_lines > 0) break;
            hsum += lh;
            keep_lines++;
        }
        tline *ln = &lines[keep_lines - 1];
        int last = ln->last;
        f32 w = ln->width;
        while (last > ln->first && w + ell > s->box_w) {
            --last;
            w -= l->g[last].adv;
        }
        l->n = last;
        for (int k = 0; k < 3 && l->n < l->cap; ++k) {
            push_glyph(l, '.', l->n > 0 ? l->g[l->n - 1].run : 0);
            l->g[l->n - 1].adv = ell / 3;
            l->g[l->n - 1].index = l->n - 1;
        }
        ln->last = l->n;
        ln->width = w + ell;
        place_lines(s, l, lines, keep_lines);
    }
    plat_free(lines);
    return true;
}

f32 ub_text_lay_out_scaled(ub_text_style *s, f32 lo, f32 hi, ub_text_layout *out)
{
    bool wrap = s->wrap;
    s->wrap = true;
    if (hi < lo) hi = lo;
    int a = (int)lo, b = (int)hi, best = a;
    /* sizes in a rich text scale with the label's own size */
    while (a <= b) {
        int m = (a + b) / 2;
        s->base.size = (f32)m;
        if (!ub_text_lay_out(s, out)) break;
        if (out->fits && !out->broke_word) { /* TextScaled never splits a word */
            best = m;
            a = m + 1;
        } else {
            b = m - 1;
        }
    }
    s->base.size = (f32)best;
    ub_text_lay_out(s, out);
    s->wrap = wrap;
    return (f32)best;
}

void ub_text_measure(const char *text, u32 len, ub_typeface f, f32 size, f32 width, f32 *w, f32 *h)
{
    ub_text_style s;
    memset(&s, 0, sizeof s);
    s.text = text;
    s.len = len;
    s.base.face = f;
    s.base.size = size;
    s.line_height = 1;
    s.box_w = width > 0 ? width : 1e9f;
    s.box_h = 1e9f;
    s.wrap = width > 0;
    ub_text_layout l;
    ub_text_layout_init(&l);
    ub_text_lay_out(&s, &l);
    *w = l.bounds_w;
    *h = l.bounds_h;
    ub_text_layout_free(&l);
}

/* -------------------------------------------------------------- drawing */

static void draw_mask_at(ub_canvas *c, const ub_xform *t, const gslot *g, f32 x, f32 y, f32 bw, f32 bh,
                         const ub_paint *p)
{
    if (!g || g->w <= 0) return;
    ub_canvas_mask(c, t, x + (f32)g->x0, y + (f32)g->y0, g_pool + g->off, g->w, g->h, bw, bh, p);
}

void ub_text_draw(ub_canvas *c, const ub_xform *t, const ub_text_layout *l, f32 box_w, f32 box_h,
                  const ub_paint *grad, i32 max_graphemes, const ub_text_stroke *stroke, f32 alpha_scale)
{
    bool plain = ub_xform_is_plain(t);
    for (int pass = 0; pass < 2; ++pass) { /* strokes under all the text, then the text */
        for (int i = 0; i < l->n; ++i) {
            const ub_tglyph *gl = &l->g[i];
            if (gl->cp == '\n' || gl->cp == ' ') continue;
            if (max_graphemes >= 0 && gl->index >= max_graphemes) continue;
            const ub_trun *r = &l->runs[gl->run];
            f32 em = em_for(r->face, r->size);
            /* whole pixels when unrotated: crisp */
            f32 x = plain ? (f32)ub_roundf(gl->x + t->tx) - t->tx : gl->x;
            f32 y = plain ? (f32)ub_roundf(gl->y + t->ty) - t->ty : gl->y;
            const gslot *g = glyph(r->face, gl->cp, em);
            ub_paint p;
            memset(&p, 0, sizeof p);
            if (grad) p = *grad;
            if (pass == 0) {
                /* rich-text stroke wins over the label's own */
                f32 th = 0, tr = 1;
                u32 col = 0;
                if (r->stroke) {
                    th = r->stroke_thickness;
                    tr = r->stroke_transparency;
                    col = r->stroke_color;
                } else if (stroke && stroke->transparency < 1) {
                    th = stroke->thickness > 0 ? stroke->thickness : 1;
                    tr = stroke->transparency;
                    col = stroke->color;
                }
                if (th <= 0 || tr >= 1) continue;
                p.color = col;
                p.alpha = (1 - tr) * (1 - r->transparency) * alpha_scale;
                int steps = th <= 1.5f ? 8 : 16;
                for (int k = 0; k < steps; ++k) {
                    f32 ang = (f32)k * UB_TWO_PI / (f32)steps;
                    draw_mask_at(c, t, g, x + ub_cosf(ang) * th, y + ub_sinf(ang) * th, box_w, box_h, &p);
                }
                continue;
            }
            p.color = r->color;
            p.alpha = (1 - r->transparency) * alpha_scale;
            draw_mask_at(c, t, g, x, y, box_w, box_h, &p);
            if (r->underline || r->strike) {
                f32 lw = r->size / 14.0f;
                if (lw < 1) lw = 1;
                f32 ly = r->underline ? y + r->size * 0.12f : y - r->size * 0.28f;
                ub_xform lt = ub_xform_mul(t, &(ub_xform){ 1, 0, 0, 1, x, ly });
                ub_canvas_rect(c, &lt, gl->adv, lw, 0, &p);
            }
        }
    }
}
