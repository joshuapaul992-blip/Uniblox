/* Uniblox - the GUI engine (see ub_gui.h).
 *
 * Each layer (a ScreenGui, BillboardGui or SurfaceGui) is laid out into a
 * flat list of elements, one per GuiObject, in tree pre-order; a second
 * list gives the drawing order (ZIndex). Layout runs when the world's
 * epoch moves (a property, name or parent changed), the screen size
 * changes or the GUI's own state does (scrolling, a page turn). Drawing
 * runs when layout ran or something only visual changed (hover, press,
 * the caret). */
#include "gui/ub_gui.h"
#include "gui/ub_image.h"
#include "gui/ub_text.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "platform/platform.h"
#include "world/ub_classes.h"
#include "crt/ub_crt.h"

#include <stddef.h>

/* ------------------------------------------------------------- classes */

enum {
    K_NONE,
    /* drawn objects */
    K_FRAME, K_SCROLL, K_TEXTLABEL, K_TEXTBUTTON, K_TEXTBOX, K_IMAGELABEL, K_IMAGEBUTTON, K_VIEWPORT, K_VIDEO, K_CANVAS,
    /* layer collectors */
    K_SCREEN, K_BILLBOARD, K_SURFACE,
    /* components */
    K_CORNER, K_STROKE, K_GRADIENT, K_PADDING, K_SCALE, K_ASPECT, K_SIZEC, K_TEXTSIZEC, K_LIST, K_GRID, K_PAGE, K_TABLE,
    K_OTHER
};
#define IS_OBJ(k) ((k) >= K_FRAME && (k) <= K_CANVAS)
#define IS_TEXT(k) ((k) == K_TEXTLABEL || (k) == K_TEXTBUTTON || (k) == K_TEXTBOX)
#define IS_IMAGE(k) ((k) == K_IMAGELABEL || (k) == K_IMAGEBUTTON)
#define IS_BUTTON(k) ((k) == K_TEXTBUTTON || (k) == K_IMAGEBUTTON)
#define IS_LAYOUT(k) ((k) == K_LIST || (k) == K_GRID || (k) == K_PAGE || (k) == K_TABLE)

static u8 classify(const char *c)
{
    static const struct { const char *n; u8 k; } t[] = {
        { "Frame", K_FRAME }, { "ScrollingFrame", K_SCROLL }, { "TextLabel", K_TEXTLABEL },
        { "TextButton", K_TEXTBUTTON }, { "TextBox", K_TEXTBOX }, { "ImageLabel", K_IMAGELABEL },
        { "ImageButton", K_IMAGEBUTTON }, { "ViewportFrame", K_VIEWPORT }, { "VideoFrame", K_VIDEO },
        { "CanvasGroup", K_CANVAS }, { "ScreenGui", K_SCREEN }, { "BillboardGui", K_BILLBOARD },
        { "SurfaceGui", K_SURFACE }, { "UICorner", K_CORNER }, { "UIStroke", K_STROKE }, { "UIGradient", K_GRADIENT },
        { "UIPadding", K_PADDING }, { "UIScale", K_SCALE }, { "UIAspectRatioConstraint", K_ASPECT },
        { "UISizeConstraint", K_SIZEC }, { "UITextSizeConstraint", K_TEXTSIZEC }, { "UIListLayout", K_LIST },
        { "UIGridLayout", K_GRID }, { "UIPageLayout", K_PAGE }, { "UITableLayout", K_TABLE },
    };
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); ++i)
        if (!ub_strcmp(t[i].n, c)) return t[i].k;
    return K_OTHER;
}

/* ---------------------------------------------------------------- keys */

typedef struct {
    u32 Size, Position, AnchorPoint, Rotation, Visible, ZIndex, LayoutOrder, SizeConstraint, AutomaticSize,
        BackgroundColor3, BackgroundTransparency, BorderColor3, BorderSizePixel, BorderMode, ClipsDescendants, Active,
        Text, TextColor3, TextSize, TextTransparency, TextStrokeColor3, TextStrokeTransparency, TextScaled,
        TextWrapped, TextXAlignment, TextYAlignment, TextTruncate, RichText, LineHeight, MaxVisibleGraphemes, Font,
        FontFace, PlaceholderText, PlaceholderColor3, MultiLine, ClearTextOnFocus, TextEditable, CursorPosition,
        Image, ImageColor3, ImageTransparency, ImageRectOffset, ImageRectSize, ScaleType, SliceCenter, SliceScale,
        TileSize, ResampleMode, HoverImage, PressedImage, AutoButtonColor, CanvasSize, CanvasPosition,
        ScrollBarThickness, ScrollBarImageColor3, ScrollBarImageTransparency, ScrollingDirection, ScrollingEnabled,
        AutomaticCanvasSize, VerticalScrollBarPosition, Enabled, DisplayOrder, IgnoreGuiInset, ZIndexBehavior,
        Adornee, AlwaysOnTop, Brightness, MaxDistance, StudsOffset, StudsOffsetWorldSpace, ExtentsOffset,
        ExtentsOffsetWorldSpace, SizeOffset, Face, PixelsPerStud, SizingMode, ZOffset, CornerRadius, Thickness, Color,
        Transparency, ApplyStrokeMode, Offset, PaddingTop, PaddingBottom, PaddingLeft, PaddingRight, Scale,
        AspectRatio, AspectType, DominantAxis, MinSize, MaxSize, MinTextSize, MaxTextSize, FillDirection,
        HorizontalAlignment, VerticalAlignment, SortOrder, Padding, CellSize, CellPadding, FillDirectionMaxCells,
        StartCorner, Circular, AbsoluteSize, AbsolutePosition, AbsoluteRotation, TextBounds, TextFits,
        AbsoluteContentSize, AbsoluteCanvasSize, AbsoluteWindowSize, AbsoluteCellSize, AbsoluteCellCount,
        CurrentPage;
} gkeys;

static const struct { size_t off; const char *name; } k_keynames[] = {
#define KN(x) { offsetof(gkeys, x), #x }
    KN(Size), KN(Position), KN(AnchorPoint), KN(Rotation), KN(Visible), KN(ZIndex), KN(LayoutOrder),
    KN(SizeConstraint), KN(AutomaticSize), KN(BackgroundColor3), KN(BackgroundTransparency), KN(BorderColor3),
    KN(BorderSizePixel), KN(BorderMode), KN(ClipsDescendants), KN(Active), KN(Text), KN(TextColor3), KN(TextSize),
    KN(TextTransparency), KN(TextStrokeColor3), KN(TextStrokeTransparency), KN(TextScaled), KN(TextWrapped),
    KN(TextXAlignment), KN(TextYAlignment), KN(TextTruncate), KN(RichText), KN(LineHeight), KN(MaxVisibleGraphemes),
    KN(Font), KN(FontFace), KN(PlaceholderText), KN(PlaceholderColor3), KN(MultiLine), KN(ClearTextOnFocus),
    KN(TextEditable), KN(CursorPosition), KN(Image), KN(ImageColor3), KN(ImageTransparency), KN(ImageRectOffset),
    KN(ImageRectSize), KN(ScaleType), KN(SliceCenter), KN(SliceScale), KN(TileSize), KN(ResampleMode),
    KN(HoverImage), KN(PressedImage), KN(AutoButtonColor), KN(CanvasSize), KN(CanvasPosition),
    KN(ScrollBarThickness), KN(ScrollBarImageColor3), KN(ScrollBarImageTransparency), KN(ScrollingDirection),
    KN(ScrollingEnabled), KN(AutomaticCanvasSize), KN(VerticalScrollBarPosition), KN(Enabled), KN(DisplayOrder),
    KN(IgnoreGuiInset), KN(ZIndexBehavior), KN(Adornee), KN(AlwaysOnTop), KN(Brightness), KN(MaxDistance),
    KN(StudsOffset), KN(StudsOffsetWorldSpace), KN(ExtentsOffset), KN(ExtentsOffsetWorldSpace), KN(SizeOffset),
    KN(Face), KN(PixelsPerStud), KN(SizingMode), KN(ZOffset), KN(CornerRadius), KN(Thickness), KN(Color),
    KN(Transparency), KN(ApplyStrokeMode), KN(Offset), KN(PaddingTop), KN(PaddingBottom), KN(PaddingLeft),
    KN(PaddingRight), KN(Scale), KN(AspectRatio), KN(AspectType), KN(DominantAxis), KN(MinSize), KN(MaxSize),
    KN(MinTextSize), KN(MaxTextSize), KN(FillDirection), KN(HorizontalAlignment), KN(VerticalAlignment),
    KN(SortOrder), KN(Padding), KN(CellSize), KN(CellPadding), KN(FillDirectionMaxCells), KN(StartCorner),
    KN(Circular), KN(AbsoluteSize), KN(AbsolutePosition), KN(AbsoluteRotation), KN(TextBounds), KN(TextFits),
    KN(AbsoluteContentSize), KN(AbsoluteCanvasSize), KN(AbsoluteWindowSize), KN(AbsoluteCellSize),
    KN(AbsoluteCellCount), KN(CurrentPage),
#undef KN
};

/* -------------------------------------------------------------- state */

typedef struct {
    i32 inst, parent;           /* parent: element index in the layer, -1 at the top */
    u8 kind;
    bool visible, active, clips;
    f32 x, y, w, h;             /* unrotated, in layer pixels */
    f32 rot;                    /* AbsoluteRotation */
    ub_xform m;                 /* layer layout coordinates -> layer pixels (ancestors' and own rotation) */
    int cx0, cy0, cx1, cy1;     /* clip in layer pixels */
    i32 z;
    f32 scale;                  /* UIScale, all ancestors' and its own */
    i32 corner, stroke, gradient, padding, textsize;
    i32 text;                   /* text layout slot, -1 */
    f32 pad[4];                 /* padding in pixels: top, bottom, left, right */
    /* ScrollingFrame */
    f32 canvas_w, canvas_h, cpx, cpy;
    bool bar_v, bar_h;
} gelem;

typedef struct {
    i32 inst;
    u8 kind;
    i32 order;                  /* DisplayOrder (screen layers) */
    f32 w, h;                   /* its canvas, pixels */
    f32 oy;                     /* screen layers: content starts here (the inset), in screen pixels */
    bool sibling;               /* ZIndexBehavior.Sibling */
    gelem *e;
    int n, cap;
    int *draw;                  /* draw order */
    ub_text_layout *texts;
    int ntexts, texts_cap;
    bool live;                  /* still found in the world */
    /* world layers */
    bool visible;               /* billboards: in front of the camera and within reach */
    bool laid_out;
    f32 laid_w, laid_h;
    u32 laid_gen;
    u32 *px;
    int pw, ph;
    bool dirty;
    u32 version;
    ub_gui_quad quad;
} glayer;

typedef struct {
    u32 cls;                    /* class name offset it was classified from */
    u8 kind;
    u32 gen;                    /* layout generation last laid out in */
    f32 x, y, w, h, rot;        /* last reported */
    f32 tw, th;
    bool fits;
    f32 cw, ch;                 /* AbsoluteContentSize / AbsoluteCanvasSize */
    f32 cellw, cellh;
    i32 cellsx, cellsy;
    f32 winw, winh;
    i32 page;
    bool known;
} gmem;

struct ub_gui {
    gkeys k;
    const ub_world *kw;         /* the world the keys belong to */
    i32 kcount;
    i32 root;                   /* PlayerGui */
    int sw, sh;                 /* screen */
    u32 epoch;                  /* the world's epoch at the last layout */
    bool need_layout;
    u32 gen;
    glayer *layers;
    int nlayers, layers_cap;
    gmem *mem;
    i32 mem_cap;
    gmem *prev;                 /* the values last reported (Changed events) */
    i32 prev_cap;
    /* screen picture */
    u32 *screen;
    int scr_w, scr_h;
    u32 scr_version;
    bool scr_dirty;
    int rects[64 * 4];
    int nrects;
    int old_rects[64 * 4];
    int nold;
    /* input */
    i32 *hover;
    int nhover, hover_cap;
    i32 pressed[3];             /* button held on this object */
    i32 focus;                  /* TextBox */
    bool caret_on;
    f64 caret_t0, now;
    f32 mx, my;                 /* last mouse (screen) */
    bool mouse_taken, wheel_taken;
    i32 drag_scroll;            /* scrollbar being dragged */
    f32 drag_off;
    int click_count;
    f64 last_click_t;
    i32 last_click_inst;
    /* events */
    ub_gui_event *ev;
    int nev, ev_cap;
    /* quads */
    ub_gui_quad *quads;
    int nquads, quads_cap;
    ub_camera cam;
    bool have_cam;
    u32 next_id;
    ub_gui_stats stats;
    /* deferred CanvasPosition clamps */
    i32 clamp_inst[16];
    f32 clamp_x[16], clamp_y[16];
    int nclamp;
};

/* -------------------------------------------------------- small helpers */

static bool grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    int nc = *cap ? *cap * 2 : 16;
    while (nc < need) nc *= 2;
    void *np = plat_alloc((size_t)nc * elem);
    if (!np) return false;
    if (*p) {
        memcpy(np, *p, (size_t)*cap * elem);
        plat_free(*p);
    }
    memset((u8 *)np + (size_t)*cap * elem, 0, (size_t)(nc - *cap) * elem);
    *p = np;
    *cap = nc;
    return true;
}

static void event(ub_gui *g, i32 inst, u8 type, i32 x, i32 y, u8 button, u8 flag, u32 key)
{
    if (g->nev >= 4096 || !grow((void **)&g->ev, &g->ev_cap, g->nev + 1, sizeof(ub_gui_event))) return;
    ub_gui_event *e = &g->ev[g->nev++];
    e->inst = inst;
    e->type = type;
    e->x = x;
    e->y = y;
    e->button = button;
    e->flag = flag;
    e->key = key;
}

static void resolve_keys(ub_gui *g, ub_world *w)
{
    if (g->kw == w && g->kcount <= w->key_count && g->kcount > 0) return;
    for (int i = 0; i < (int)(sizeof k_keynames / sizeof k_keynames[0]); ++i)
        *(u32 *)((u8 *)&g->k + k_keynames[i].off) = ub_world_key(w, k_keynames[i].name);
    g->kw = w;
    g->kcount = w->key_count;
}

static gmem *mem_of(ub_gui *g, const ub_world *w, i32 inst)
{
    if (inst < 0 || inst >= w->count) return NULL;
    if (inst >= g->mem_cap && !grow((void **)&g->mem, &g->mem_cap, w->count, sizeof(gmem))) return NULL;
    return &g->mem[inst];
}

static u8 kind_of(ub_gui *g, const ub_world *w, i32 inst)
{
    if (!ub_world_alive(w, inst)) return K_NONE;
    gmem *m = mem_of(g, w, inst);
    u32 cn = w->inst[inst].class_name;
    if (!m) return classify(ub_world_str(w, cn));
    if (m->cls != cn + 1) {
        memset(m, 0, sizeof *m);
        m->cls = cn + 1;
        m->kind = classify(ub_world_str(w, cn));
        m->page = -1;
    }
    return m->kind;
}

/* property reads, with defaults for anything missing or of another type */
static const ub_value *P(const ub_world *w, i32 i, u32 k) { return ub_world_prop(w, i, k); }

static f32 pnum(const ub_world *w, i32 i, u32 k, f32 def)
{
    const ub_value *v = P(w, i, k);
    if (!v) return def;
    f32 r = def;
    if (v->type == UB_PV_FLOAT) r = v->v.f;
    else if (v->type == UB_PV_DOUBLE) r = (f32)v->v.d;
    else if (v->type == UB_PV_INT) r = (f32)v->v.i;
    return r == r ? r : def;
}

static int pint(const ub_world *w, i32 i, u32 k, int def)
{
    const ub_value *v = P(w, i, k);
    if (!v) return def;
    if (v->type == UB_PV_INT || v->type == UB_PV_ENUM || v->type == UB_PV_BRICKCOLOR) return v->v.i;
    if (v->type == UB_PV_BOOL) return v->v.b;
    if (v->type == UB_PV_FLOAT) return (int)v->v.f;
    if (v->type == UB_PV_DOUBLE) return (int)v->v.d;
    return def;
}

static bool pbool(const ub_world *w, i32 i, u32 k, bool def)
{
    const ub_value *v = P(w, i, k);
    return v && v->type == UB_PV_BOOL ? v->v.b : def;
}

static u32 to8(f32 c) { return (u32)(ub_clampf(c, 0, 1) * 255.0f + 0.5f); }

static u32 pcolor(const ub_world *w, i32 i, u32 k, u32 def)
{
    const ub_value *v = P(w, i, k);
    if (!v || v->type != UB_PV_COLOR3) return def;
    return (to8(v->v.c3.r) << 16) | (to8(v->v.c3.g) << 8) | to8(v->v.c3.b);
}

static void pv2(const ub_world *w, i32 i, u32 k, f32 *x, f32 *y, f32 dx, f32 dy)
{
    const ub_value *v = P(w, i, k);
    if (v && v->type == UB_PV_VECTOR2) {
        *x = v->v.v2.x;
        *y = v->v.v2.y;
    } else {
        *x = dx;
        *y = dy;
    }
}

static vec3 pv3(const ub_world *w, i32 i, u32 k)
{
    const ub_value *v = P(w, i, k);
    return v && v->type == UB_PV_VECTOR3 ? v3(v->v.v3.x, v->v.v3.y, v->v.v3.z) : v3(0, 0, 0);
}

typedef struct { f32 sx, ox, sy, oy; } udim2;

static udim2 pud2(const ub_world *w, i32 i, u32 k, udim2 def)
{
    const ub_value *v = P(w, i, k);
    if (!v || v->type != UB_PV_UDIM2) return def;
    udim2 u = { v->v.ud2.sx, (f32)v->v.ud2.ox, v->v.ud2.sy, (f32)v->v.ud2.oy };
    return u;
}

static void pud(const ub_world *w, i32 i, u32 k, f32 *s, f32 *o)
{
    const ub_value *v = P(w, i, k);
    *s = v && v->type == UB_PV_UDIM ? v->v.ud.s : 0;
    *o = v && v->type == UB_PV_UDIM ? (f32)v->v.ud.o : 0;
}

static const char *pstr(const ub_world *w, i32 i, u32 k, u32 *len)
{
    const ub_value *v = P(w, i, k);
    if (!v || v->type != UB_PV_STRING) {
        *len = 0;
        return "";
    }
    *len = v->v.str.len;
    return v->v.str.s;
}

static i32 pref(const ub_world *w, i32 i, u32 k)
{
    const ub_value *v = P(w, i, k);
    return v && v->type == UB_PV_REF && ub_world_alive(w, v->v.ref) ? v->v.ref : -1;
}

static f32 fin(f32 v, f32 lim) { return v == v ? ub_clampf(v, -lim, lim) : 0; }

/* ------------------------------------------------------------ creation */

ub_gui *ub_gui_new(void)
{
    ub_gui *g = (ub_gui *)plat_alloc(sizeof(ub_gui));
    if (!g) return NULL;
    memset(g, 0, sizeof *g);
    g->root = -1;
    g->focus = -1;
    g->drag_scroll = -1;
    g->pressed[0] = g->pressed[1] = g->pressed[2] = -1;
    g->last_click_inst = -1;
    g->need_layout = true;
    g->next_id = 0x6A000000u;
    return g;
}

static void free_layer(glayer *l)
{
    for (int i = 0; i < l->texts_cap; ++i) ub_text_layout_free(&l->texts[i]);
    plat_free(l->texts);
    plat_free(l->e);
    plat_free(l->draw);
    plat_free(l->px);
    memset(l, 0, sizeof *l);
}

void ub_gui_free(ub_gui *g)
{
    if (!g) return;
    for (int i = 0; i < g->nlayers; ++i) free_layer(&g->layers[i]);
    plat_free(g->layers);
    plat_free(g->mem);
    plat_free(g->prev);
    plat_free(g->screen);
    plat_free(g->hover);
    plat_free(g->ev);
    plat_free(g->quads);
    plat_free(g);
}

void ub_gui_set_root(ub_gui *g, i32 player_gui)
{
    if (g->root != player_gui) g->need_layout = true;
    g->root = player_gui;
}

int ub_gui_events(const ub_gui *g, const ub_gui_event **out)
{
    *out = g->ev;
    return g->nev;
}

void ub_gui_clear_events(ub_gui *g) { g->nev = 0; }

/* ------------------------------------------------------------ layers */

static glayer *find_layer(ub_gui *g, i32 inst)
{
    for (int i = 0; i < g->nlayers; ++i)
        if (g->layers[i].inst == inst) return &g->layers[i];
    return NULL;
}

static void add_layer(ub_gui *g, i32 inst, u8 kind)
{
    glayer *l = find_layer(g, inst);
    if (l && l->kind == kind) {
        l->live = true;
        return;
    }
    if (l) free_layer(l);
    if (!l) {
        if (g->nlayers >= 4096 || !grow((void **)&g->layers, &g->layers_cap, g->nlayers + 1, sizeof(glayer))) return;
        l = &g->layers[g->nlayers++];
        memset(l, 0, sizeof *l);
    }
    l->inst = inst;
    l->kind = kind;
    l->live = true;
    l->dirty = true;
    l->quad.image.id = g->next_id++;
}

/* LayerCollectors: under the PlayerGui (through Folders and the like, not
 * inside GuiObjects), and BillboardGuis / SurfaceGuis in the Workspace. */
static void find_layers(ub_gui *g, const ub_world *w, i32 root, bool screen_ok)
{
    i32 stack[256];
    int sp = 0;
    if (root < 0 || !ub_world_alive(w, root)) return;
    for (i32 c = w->inst[root].first_child; c >= 0 && sp < 256; c = w->inst[c].next_sibling) stack[sp++] = c;
    while (sp > 0) {
        i32 i = stack[--sp];
        if (!ub_world_alive(w, i)) continue;
        u8 k = kind_of(g, w, i);
        if (k == K_SCREEN && screen_ok) {
            add_layer(g, i, k);
            continue;
        }
        if (k == K_BILLBOARD || k == K_SURFACE) {
            add_layer(g, i, k);
            continue;
        }
        if (IS_OBJ(k) || k == K_SCREEN) continue; /* GUI inside a GuiObject is not a layer */
        for (i32 c = w->inst[i].first_child; c >= 0 && sp < 256; c = w->inst[c].next_sibling) stack[sp++] = c;
    }
}

static void collect_layers(ub_gui *g, ub_world *w)
{
    for (int i = 0; i < g->nlayers; ++i) g->layers[i].live = false;
    find_layers(g, w, g->root, true);
    if (w->workspace >= 0) find_layers(g, w, w->workspace, false);
    /* drop the ones that went */
    for (int i = 0; i < g->nlayers;) {
        if (!g->layers[i].live) {
            if (g->layers[i].kind == K_SCREEN) g->scr_dirty = true;
            free_layer(&g->layers[i]);
            g->layers[i] = g->layers[--g->nlayers];
            memset(&g->layers[g->nlayers], 0, sizeof(glayer));
        } else {
            ++i;
        }
    }
}

/* ---------------------------------------------------------- layout */

typedef struct {
    ub_gui *g;
    ub_world *w;
    glayer *l;
} lctx;

static f32 cornerpx(lctx *c, i32 corner, f32 w, f32 h, f32 scale)
{
    if (corner < 0) return 0;
    f32 s, o;
    pud(c->w, corner, c->g->k.CornerRadius, &s, &o);
    f32 r = s * (w < h ? w : h) + o * scale;
    return r > 0 ? r : 0;
}

/* The children of `inst` that matter to layout. */
typedef struct {
    i32 objs[512];
    int n;
    i32 layout, padding, scale, aspect, sizec, textsize, corner, stroke, gradient;
} kids;

static void get_kids(lctx *c, i32 inst, kids *k)
{
    k->n = 0;
    k->layout = k->padding = k->scale = k->aspect = k->sizec = k->textsize = k->corner = k->stroke = k->gradient = -1;
    const ub_world *w = c->w;
    for (i32 ch = w->inst[inst].first_child; ch >= 0; ch = w->inst[ch].next_sibling) {
        if (!ub_world_alive(w, ch)) continue;
        u8 kk = kind_of(c->g, w, ch);
        if (IS_OBJ(kk)) {
            if (k->n < 512) k->objs[k->n++] = ch;
        } else if (IS_LAYOUT(kk) && k->layout < 0) k->layout = ch;
        else if (kk == K_PADDING && k->padding < 0) k->padding = ch;
        else if (kk == K_SCALE && k->scale < 0) k->scale = ch;
        else if (kk == K_ASPECT && k->aspect < 0) k->aspect = ch;
        else if (kk == K_SIZEC && k->sizec < 0) k->sizec = ch;
        else if (kk == K_TEXTSIZEC && k->textsize < 0) k->textsize = ch;
        else if (kk == K_CORNER && k->corner < 0) k->corner = ch;
        else if (kk == K_STROKE && k->stroke < 0 && pbool(w, ch, c->g->k.Enabled, true)) k->stroke = ch;
        else if (kk == K_GRADIENT && k->gradient < 0 && pbool(w, ch, c->g->k.Enabled, true)) k->gradient = ch;
    }
}

static void padding_of(lctx *c, i32 pad, f32 w, f32 h, f32 scale, f32 out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (pad < 0) return;
    const gkeys *k = &c->g->k;
    f32 s, o;
    pud(c->w, pad, k->PaddingTop, &s, &o);    out[0] = s * h + o * scale;
    pud(c->w, pad, k->PaddingBottom, &s, &o); out[1] = s * h + o * scale;
    pud(c->w, pad, k->PaddingLeft, &s, &o);   out[2] = s * w + o * scale;
    pud(c->w, pad, k->PaddingRight, &s, &o);  out[3] = s * w + o * scale;
}

/* A text object's style: its font, size and alignment. */
static void text_style(lctx *c, i32 inst, u8 kind, f32 bw, f32 bh, f32 scale, ub_text_style *s, bool *scaled)
{
    const ub_world *w = c->w;
    const gkeys *k = &c->g->k;
    memset(s, 0, sizeof *s);
    u32 len;
    const char *t = pstr(w, inst, k->Text, &len);
    if (kind == K_TEXTBOX && len == 0) t = pstr(w, inst, k->PlaceholderText, &len);
    s->text = t;
    s->len = len;
    s->rich = pbool(w, inst, k->RichText, false) && kind != K_TEXTBOX;
    const ub_value *ff = P(w, inst, k->FontFace);
    if (ff && ff->type == UB_PV_FONT) s->base.face = ub_text_face_family(ff->v.font.family, ff->v.font.weight, ff->v.font.style);
    else s->base.face = ub_text_face_enum(pint(w, inst, k->Font, 0));
    s->base.size = fin(pnum(w, inst, k->TextSize, 8), 1000) * scale;
    if (s->base.size < 1) s->base.size = 1;
    s->base.color = pcolor(w, inst, k->TextColor3, 0x1B2A35);
    if (kind == K_TEXTBOX && pstr(w, inst, k->Text, &len) && len == 0) s->base.color = pcolor(w, inst, k->PlaceholderColor3, 0xB2B2B2);
    s->base.transparency = ub_clampf(pnum(w, inst, k->TextTransparency, 0), 0, 1);
    s->line_height = ub_clampf(pnum(w, inst, k->LineHeight, 1), 1, 3);
    s->box_w = bw;
    s->box_h = bh;
    s->xalign = pint(w, inst, k->TextXAlignment, 2);
    s->yalign = pint(w, inst, k->TextYAlignment, 1);
    s->wrap = pbool(w, inst, k->TextWrapped, false) || (kind == K_TEXTBOX && pbool(w, inst, k->MultiLine, false));
    s->truncate = pint(w, inst, k->TextTruncate, 0) == 1;
    *scaled = pbool(w, inst, k->TextScaled, false);
}

static ub_text_layout *text_slot(glayer *l, int slot)
{
    if (slot >= l->texts_cap) {
        int old = l->texts_cap;
        if (!grow((void **)&l->texts, &l->texts_cap, slot + 1, sizeof(ub_text_layout))) return NULL;
        for (int i = old; i < l->texts_cap; ++i) ub_text_layout_init(&l->texts[i]);
    }
    return &l->texts[slot];
}

/* Lays out a text object's text in its (padded) box. */
static void lay_text(lctx *c, gelem *e, i32 textsizec)
{
    glayer *l = c->l;
    int slot = l->ntexts;
    ub_text_layout *tl = text_slot(l, slot);
    if (!tl) return;
    l->ntexts++;
    ub_text_style s;
    bool scaled;
    f32 bw = e->w - e->pad[2] - e->pad[3], bh = e->h - e->pad[0] - e->pad[1];
    text_style(c, e->inst, e->kind, bw > 0 ? bw : 0, bh > 0 ? bh : 0, e->scale, &s, &scaled);
    if (scaled) {
        f32 lo = 1, hi = 100;
        if (textsizec >= 0) {
            lo = ub_clampf(pnum(c->w, textsizec, c->g->k.MinTextSize, 1), 1, 100);
            hi = ub_clampf(pnum(c->w, textsizec, c->g->k.MaxTextSize, 100), 1, 100);
        }
        ub_text_lay_out_scaled(&s, lo * e->scale, hi * e->scale, tl);
    } else {
        if (textsizec >= 0) {
            f32 lo = pnum(c->w, textsizec, c->g->k.MinTextSize, 1), hi = pnum(c->w, textsizec, c->g->k.MaxTextSize, 100);
            s.base.size = ub_clampf(s.base.size, lo * e->scale, (hi > lo ? hi : lo) * e->scale);
        }
        ub_text_lay_out(&s, tl);
    }
    e->text = slot;
}

/* The pixel size of a child of a container of content size (cw, ch). */
static void child_size(lctx *c, i32 inst, f32 cw, f32 ch, f32 scale, const kids *k, f32 *w_out, f32 *h_out)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    udim2 z = { 0, 0, 0, 0 };
    udim2 s = pud2(w, inst, K->Size, z);
    int sc = pint(w, inst, K->SizeConstraint, 0);
    f32 rx = sc == 2 ? ch : cw, ry = sc == 1 ? cw : ch;
    f32 W = s.sx * rx + s.ox * scale, H = s.sy * ry + s.oy * scale;
    if (k->scale >= 0) {
        f32 u = fin(pnum(w, k->scale, K->Scale, 1), 1000);
        W *= u;
        H *= u;
    }
    if (k->aspect >= 0) {
        f32 ar = pnum(w, k->aspect, K->AspectRatio, 1);
        if (ar > 1e-4f && ar < 1e4f) {
            int type = pint(w, k->aspect, K->AspectType, 0), dom = pint(w, k->aspect, K->DominantAxis, 0);
            if (type == 0) { /* FitWithinMaxSize */
                if (H > 0 && W / H > ar) W = H * ar;
                else H = W / ar;
            } else if (dom == 0) {
                H = W / ar;
            } else {
                W = H * ar;
            }
        }
    }
    if (k->sizec >= 0) {
        f32 minx, miny, maxx, maxy;
        pv2(w, k->sizec, K->MinSize, &minx, &miny, 0, 0);
        pv2(w, k->sizec, K->MaxSize, &maxx, &maxy, 1e9f, 1e9f);
        W = ub_clampf(W, minx, maxx > minx ? maxx : minx);
        H = ub_clampf(H, miny, maxy > miny ? maxy : miny);
    }
    *w_out = fin(W, 1e6f);
    *h_out = fin(H, 1e6f);
}

static void lay_children(lctx *c, i32 parent_inst, int parent_elem, f32 cx, f32 cy, f32 cw, f32 ch, const ub_xform *m,
                         f32 rot, f32 scale, const int clip[4], bool visible, bool emit, f32 *ext_w, f32 *ext_h);

/* A GuiObject's size with AutomaticSize applied (its children and text
 * measured). */
static void auto_size(lctx *c, i32 inst, u8 kind, const kids *k, f32 scale, f32 *W, f32 *H)
{
    int as = pint(c->w, inst, c->g->k.AutomaticSize, 0);
    if (as == 0 || kind == K_SCROLL) return;
    f32 pad[4];
    padding_of(c, k->padding, *W, *H, scale, pad);
    f32 ew = 0, eh = 0;
    ub_xform id = ub_xform_place(0, 0);
    int clip[4] = { -100000, -100000, 100000, 100000 };
    lay_children(c, inst, -1, pad[2], pad[0], *W - pad[2] - pad[3], *H - pad[0] - pad[1], &id, 0, scale, clip, false,
                 false, &ew, &eh);
    ew += pad[2] + pad[3];
    eh += pad[0] + pad[1];
    if (IS_TEXT(kind)) {
        ub_text_style s;
        bool scaled;
        ub_text_layout tl;
        ub_text_layout_init(&tl);
        /* along an automatic axis the text does not wrap at the box */
        f32 bw = (as == 1 || as == 3) ? 1e6f : *W - pad[2] - pad[3];
        text_style(c, inst, kind, bw, 1e6f, scale, &s, &scaled);
        ub_text_lay_out(&s, &tl);
        if (tl.bounds_w + pad[2] + pad[3] > ew) ew = tl.bounds_w + pad[2] + pad[3];
        if (tl.bounds_h + pad[0] + pad[1] > eh) eh = tl.bounds_h + pad[0] + pad[1];
        ub_text_layout_free(&tl);
    }
    if ((as == 1 || as == 3) && ew > *W) *W = ew;
    if ((as == 2 || as == 3) && eh > *H) *H = eh;
}

static int cmp_name(const ub_world *w, i32 a, i32 b)
{
    return ub_strcmp(ub_world_str(w, w->inst[a].name), ub_world_str(w, w->inst[b].name));
}

/* Children in the order a layout places them. */
static void sort_for_layout(lctx *c, i32 layout, i32 *v, int n)
{
    int order = pint(c->w, layout, c->g->k.SortOrder, 2); /* 0 Name, 1 Custom, 2 LayoutOrder */
    if (order == 1) return;
    for (int i = 1; i < n; ++i) { /* insertion sort: stable, and lists are short */
        i32 x = v[i];
        int j = i - 1;
        while (j >= 0) {
            int d = order == 0 ? cmp_name(c->w, v[j], x)
                               : pint(c->w, v[j], c->g->k.LayoutOrder, 0) - pint(c->w, x, c->g->k.LayoutOrder, 0);
            if (d <= 0) break;
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
}

static gmem *note(lctx *c, i32 inst) { return mem_of(c->g, c->w, inst); }

/* Emits one element and lays out what is inside it. */
static void emit_elem(lctx *c, i32 inst, u8 kind, const kids *k, int parent_elem, f32 x, f32 y, f32 W, f32 H,
                      const ub_xform *pm, f32 prot, f32 pscale, const int pclip[4], bool pvis, bool emit,
                      f32 *ext_w, f32 *ext_h)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    f32 own_scale = k->scale >= 0 ? fin(pnum(w, k->scale, K->Scale, 1), 1000) : 1;
    f32 scale = pscale * own_scale;
    f32 rot = fin(pnum(w, inst, K->Rotation, 0), 1e6f);
    bool vis = pvis && pbool(w, inst, K->Visible, true);
    if (ext_w && x + W > *ext_w) *ext_w = x + W;
    if (ext_h && y + H > *ext_h) *ext_h = y + H;
    if (!emit) return;
    glayer *l = c->l;
    if (l->n >= 200000 || !grow((void **)&l->e, &l->cap, l->n + 1, sizeof(gelem))) return;
    int ei = l->n++;
    gelem *e = &l->e[ei];
    memset(e, 0, sizeof *e);
    e->inst = inst;
    e->parent = parent_elem;
    e->kind = kind;
    e->visible = vis;
    e->x = x;
    e->y = y;
    e->w = W > 0 ? W : 0;
    e->h = H > 0 ? H : 0;
    e->rot = prot + rot;
    e->scale = scale;
    e->z = pint(w, inst, K->ZIndex, 1);
    e->active = pbool(w, inst, K->Active, IS_BUTTON(kind) || kind == K_TEXTBOX);
    e->corner = k->corner;
    e->stroke = k->stroke;
    e->gradient = k->gradient;
    e->padding = k->padding;
    e->textsize = k->textsize;
    e->text = -1;
    /* rotation about its centre, on top of its ancestors' */
    ub_xform r = ub_xform_rotated(x, y, e->w, e->h, rot);
    ub_xform rr = { r.a, r.b, r.c, r.d, r.tx - r.a * x - r.c * y, r.ty - r.b * x - r.d * y }; /* in layout space */
    e->m = rot != 0 ? ub_xform_mul(pm, &rr) : *pm;
    e->cx0 = pclip[0];
    e->cy0 = pclip[1];
    e->cx1 = pclip[2];
    e->cy1 = pclip[3];
    padding_of(c, k->padding, e->w, e->h, scale, e->pad);
    gmem *m = note(c, inst);
    if (m) {
        m->gen = c->g->gen;
        m->x = x;
        m->y = y;
        m->w = e->w;
        m->h = e->h;
        m->rot = e->rot;
    }
    if (IS_TEXT(kind)) {
        lay_text(c, e, k->textsize);
        if (m && e->text >= 0) {
            m->tw = l->texts[e->text].bounds_w;
            m->th = l->texts[e->text].bounds_h;
            m->fits = l->texts[e->text].fits;
        }
    }
    /* children: clipped to this object if it clips (and nothing is rotated) */
    int clip[4] = { pclip[0], pclip[1], pclip[2], pclip[3] };
    bool clips = kind == K_SCROLL || pbool(w, inst, K->ClipsDescendants, false);
    if (clips && e->rot == 0 && ub_xform_is_plain(pm)) {
        f32 X = pm->tx + x, Y = pm->ty + y;
        clip[0] = UB_MAX(clip[0], ub_roundf(X));
        clip[1] = UB_MAX(clip[1], ub_roundf(Y));
        clip[2] = UB_MIN(clip[2], ub_roundf(X + e->w));
        clip[3] = UB_MIN(clip[3], ub_roundf(Y + e->h));
    }
    e->clips = clips;
    f32 ox = x + e->pad[2], oy = y + e->pad[0];
    f32 iw = e->w - e->pad[2] - e->pad[3], ih = e->h - e->pad[0] - e->pad[1];
    ub_xform cm = e->m;
    if (kind == K_SCROLL) {
        udim2 dz = { 0, 0, 2, 0 };
        udim2 cs = pud2(w, inst, K->CanvasSize, dz);
        f32 canw = cs.sx * e->w + cs.ox * scale, canh = cs.sy * e->h + cs.oy * scale;
        int acs = pint(w, inst, K->AutomaticCanvasSize, 0);
        if (acs) { /* the content's extent, measured first */
            f32 ew = 0, eh = 0;
            ub_xform id = ub_xform_place(0, 0);
            int big[4] = { -1000000, -1000000, 1000000, 1000000 };
            lay_children(c, inst, -1, e->pad[2], e->pad[0], iw, ih, &id, 0, scale, big, false, false, &ew, &eh);
            ew += e->pad[3];
            eh += e->pad[1];
            if ((acs == 1 || acs == 3) && ew > canw) canw = ew;
            if ((acs == 2 || acs == 3) && eh > canh) canh = eh;
        }
        /* the canvas is never smaller than the window */
        if (canw < e->w) canw = e->w;
        if (canh < e->h) canh = e->h;
        f32 cpx, cpy;
        pv2(w, inst, K->CanvasPosition, &cpx, &cpy, 0, 0);
        int dir = pint(w, inst, K->ScrollingDirection, 4);
        f32 maxx = canw - e->w > 0 ? canw - e->w : 0, maxy = canh - e->h > 0 ? canh - e->h : 0;
        f32 ncx = ub_clampf(cpx, 0, maxx), ncy = ub_clampf(cpy, 0, maxy);
        if ((ncx != cpx || ncy != cpy) && c->g->nclamp < 16) {
            c->g->clamp_inst[c->g->nclamp] = inst;
            c->g->clamp_x[c->g->nclamp] = ncx;
            c->g->clamp_y[c->g->nclamp++] = ncy;
        }
        e->canvas_w = canw;
        e->canvas_h = canh;
        e->cpx = ncx;
        e->cpy = ncy;
        e->bar_v = (dir == 2 || dir == 4) && canh > e->h + 0.5f;
        e->bar_h = (dir == 1 || dir == 4) && canw > e->w + 0.5f;
        if (m) {
            m->cw = canw;
            m->ch = canh;
            m->winw = e->w;
            m->winh = e->h;
        }
        ox = x - ncx + e->pad[2];
        oy = y - ncy + e->pad[0];
        iw = canw - e->pad[2] - e->pad[3];
        ih = canh - e->pad[0] - e->pad[1];
    }
    lay_children(c, inst, ei, ox, oy, iw, ih, &cm, e->rot, scale, clip, vis, true, NULL, NULL);
}

static void lay_children(lctx *c, i32 parent_inst, int parent_elem, f32 cx, f32 cy, f32 cw, f32 ch, const ub_xform *m,
                         f32 rot, f32 scale, const int clip[4], bool visible, bool emit, f32 *ext_w, f32 *ext_h)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    kids pk;
    get_kids(c, parent_inst, &pk);
    if (!pk.n) return;
    /* everything this needs is copied out of `pk`, which is large: keep it on the heap */
    int n = pk.n;
    i32 *v = (i32 *)plat_alloc(sizeof(i32) * (size_t)n * 2);
    f32 *sz = (f32 *)plat_alloc(sizeof(f32) * (size_t)n * 2);
    kids *ck = (kids *)plat_alloc(sizeof(kids));
    if (!v || !sz || !ck) {
        plat_free(v);
        plat_free(sz);
        plat_free(ck);
        return;
    }
    memcpy(v, pk.objs, sizeof(i32) * (size_t)n);
    i32 layout = pk.layout;
    u8 lk = layout >= 0 ? kind_of(c->g, w, layout) : K_NONE;
    /* the order: a layout's, or ZIndex among siblings */
    if (layout >= 0 && lk != K_TABLE) sort_for_layout(c, layout, v, n);
    for (int i = 0; i < n; ++i) {
        get_kids(c, v[i], ck);
        f32 W, H;
        child_size(c, v[i], cw, ch, scale, ck, &W, &H);
        auto_size(c, v[i], kind_of(c->g, w, v[i]), ck, scale * (ck->scale >= 0 ? pnum(w, ck->scale, K->Scale, 1) : 1), &W, &H);
        sz[i * 2] = W;
        sz[i * 2 + 1] = H;
    }
    f32 *pos = (f32 *)plat_alloc(sizeof(f32) * (size_t)n * 2);
    if (!pos) goto done;
    if (layout >= 0 && (lk == K_LIST || lk == K_TABLE || lk == K_PAGE)) {
        int dir = pint(w, layout, K->FillDirection, 1); /* 0 horizontal, 1 vertical */
        int ha = pint(w, layout, K->HorizontalAlignment, 1), va = pint(w, layout, K->VerticalAlignment, 1);
        f32 ps, po;
        pud(w, layout, K->Padding, &ps, &po);
        f32 pad = ps * (dir ? ch : cw) + po * scale;
        f32 along = 0, across = 0;
        bool page = lk == K_PAGE;
        int cur = 0;
        gmem *lm = note(c, layout);
        if (page && lm) {
            for (int i = 0; i < n; ++i)
                if (v[i] == lm->page) cur = i;
            if (lm->page < 0 || cur >= n) lm->page = v[0];
        }
        /* pages: each takes the whole content, one after another */
        int visible_n = 0;
        for (int i = 0; i < n; ++i) {
            bool vis = pbool(w, v[i], K->Visible, true);
            if (!vis && !page) {
                pos[i * 2] = cx;
                pos[i * 2 + 1] = cy;
                continue;
            }
            f32 W = sz[i * 2], H = sz[i * 2 + 1];
            f32 a = dir ? W : H;
            f32 p = along;
            along += (dir ? H : W) + pad;
            if (a > across) across = a;
            pos[i * 2] = dir ? 0 : p;
            pos[i * 2 + 1] = dir ? p : 0;
            visible_n++;
        }
        if (visible_n) along -= pad;
        f32 total_w = dir ? across : along, total_h = dir ? along : across;
        f32 shift_x = 0, shift_y = 0;
        if (page) { /* the current page fills the content */
            shift_x = dir ? 0 : -pos[cur * 2];
            shift_y = dir ? -pos[cur * 2 + 1] : 0;
        } else {
            f32 sx_space = cw - total_w, sy_space = ch - total_h;
            if (!dir) shift_x = ha == 0 ? sx_space * 0.5f : ha == 2 ? sx_space : 0;
            if (dir) shift_y = va == 0 ? sy_space * 0.5f : va == 2 ? sy_space : 0;
        }
        for (int i = 0; i < n; ++i) {
            f32 W = sz[i * 2], H = sz[i * 2 + 1];
            if (dir) { /* across: alignment per child */
                pos[i * 2] = cx + (ha == 0 ? (cw - W) * 0.5f : ha == 2 ? cw - W : 0);
                pos[i * 2 + 1] += cy + shift_y;
            } else {
                pos[i * 2 + 1] = cy + (va == 0 ? (ch - H) * 0.5f : va == 2 ? ch - H : 0);
                pos[i * 2] += cx + shift_x;
            }
        }
        if (lm) {
            lm->gen = c->g->gen;
            lm->cw = total_w;
            lm->ch = total_h;
        }
    } else if (layout >= 0 && lk == K_GRID) {
        udim2 dcs = { 0, 100, 0, 100 }, dcp = { 0, 5, 0, 5 };
        udim2 cs = pud2(w, layout, K->CellSize, dcs), cp = pud2(w, layout, K->CellPadding, dcp);
        f32 cellw = cs.sx * cw + cs.ox * scale, cellh = cs.sy * ch + cs.oy * scale;
        f32 padx = cp.sx * cw + cp.ox * scale, pady = cp.sy * ch + cp.oy * scale;
        int dir = pint(w, layout, K->FillDirection, 0);
        int maxc = pint(w, layout, K->FillDirectionMaxCells, 0);
        int corner = pint(w, layout, K->StartCorner, 0);
        int ha = pint(w, layout, K->HorizontalAlignment, 1), va = pint(w, layout, K->VerticalAlignment, 1);
        int fit = dir == 0 ? (int)((cw + padx) / (cellw + padx > 0.01f ? cellw + padx : 1))
                           : (int)((ch + pady) / (cellh + pady > 0.01f ? cellh + pady : 1));
        if (fit < 1) fit = 1;
        if (maxc > 0 && maxc < fit) fit = maxc;
        int count = 0;
        for (int i = 0; i < n; ++i) count += pbool(w, v[i], K->Visible, true);
        int cols = dir == 0 ? UB_MIN(fit, UB_MAX(count, 1)) : (count + fit - 1) / fit;
        int rows = dir == 0 ? (count + fit - 1) / fit : UB_MIN(fit, UB_MAX(count, 1));
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        f32 tw = cols * cellw + (cols - 1) * padx, th = rows * cellh + (rows - 1) * pady;
        f32 bx = cx + (ha == 0 ? (cw - tw) * 0.5f : ha == 2 ? cw - tw : 0);
        f32 by = cy + (va == 0 ? (ch - th) * 0.5f : va == 2 ? ch - th : 0);
        int idx = 0;
        for (int i = 0; i < n; ++i) {
            sz[i * 2] = cellw;
            sz[i * 2 + 1] = cellh;
            if (!pbool(w, v[i], K->Visible, true)) {
                pos[i * 2] = bx;
                pos[i * 2 + 1] = by;
                continue;
            }
            int col = dir == 0 ? idx % fit : idx / fit, row = dir == 0 ? idx / fit : idx % fit;
            if (corner == 1 || corner == 3) col = cols - 1 - col;
            if (corner == 2 || corner == 3) row = rows - 1 - row;
            pos[i * 2] = bx + col * (cellw + padx);
            pos[i * 2 + 1] = by + row * (cellh + pady);
            idx++;
        }
        gmem *lm = note(c, layout);
        if (lm) {
            lm->gen = c->g->gen;
            lm->cw = tw;
            lm->ch = th;
            lm->cellw = cellw;
            lm->cellh = cellh;
            lm->cellsx = cols;
            lm->cellsy = rows;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            udim2 z = { 0, 0, 0, 0 };
            udim2 p = pud2(w, v[i], K->Position, z);
            f32 ax, ay;
            pv2(w, v[i], K->AnchorPoint, &ax, &ay, 0, 0);
            pos[i * 2] = cx + p.sx * cw + p.ox * scale - ax * sz[i * 2];
            pos[i * 2 + 1] = cy + p.sy * ch + p.oy * scale - ay * sz[i * 2 + 1];
        }
    }
    /* emitting: in ZIndex order among siblings for ZIndexBehavior.Sibling */
    int *ord = (int *)plat_alloc(sizeof(int) * (size_t)n);
    if (ord) {
        for (int i = 0; i < n; ++i) ord[i] = i;
        if (emit && c->l->sibling)
            for (int i = 1; i < n; ++i) {
                int x = ord[i], j = i - 1;
                int zx = pint(w, v[x], K->ZIndex, 1);
                while (j >= 0 && pint(w, v[ord[j]], K->ZIndex, 1) > zx) {
                    ord[j + 1] = ord[j];
                    --j;
                }
                ord[j + 1] = x;
            }
        for (int q = 0; q < n; ++q) {
            int i = ord[q];
            get_kids(c, v[i], ck);
            f32 ex = 0, ey = 0;
            emit_elem(c, v[i], kind_of(c->g, w, v[i]), ck, parent_elem, pos[i * 2], pos[i * 2 + 1], sz[i * 2],
                      sz[i * 2 + 1], m, rot, scale, clip, visible, emit, &ex, &ey);
            /* extents relative to the content origin, of visible children */
            if (ext_w && pbool(w, v[i], K->Visible, true) && ex - cx > *ext_w) *ext_w = ex - cx;
            if (ext_h && pbool(w, v[i], K->Visible, true) && ey - cy > *ext_h) *ext_h = ey - cy;
        }
        plat_free(ord);
    }
    plat_free(pos);
done:
    plat_free(v);
    plat_free(sz);
    plat_free(ck);
}

/* Projects a world point (Uniblox axes) through the camera; false if
 * behind. Also gives the pixels per stud at that depth. */
static bool project(const ub_camera *cam, int sw, int sh, vec3 p, f32 *x, f32 *y, f32 *ppu, f32 *depth)
{
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw), sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    vec3 d = v3_sub(p, cam->pos);
    f32 z = v3_dot(d, fwd);
    if (z < 0.1f) return false;
    f32 t = ub_sinf(cam->fov_y * 0.5f) / ub_cosf(cam->fov_y * 0.5f);
    f32 h = (f32)(sh > 0 ? sh : 1);
    *ppu = h * 0.5f / (z * t);
    *x = (f32)sw * 0.5f + v3_dot(d, right) * *ppu;
    *y = (f32)sh * 0.5f - v3_dot(d, up) * *ppu;
    *depth = z;
    return true;
}

/* A part's pose in Roblox axes: position and rotation columns. */
static bool part_pose(const ub_world *w, i32 inst, vec3 *pos, mat3 *rot, vec3 *size)
{
    if (!ub_world_alive(w, inst) || w->inst[inst].part < 0) return false;
    const ub_part *p = &w->parts[w->inst[inst].part];
    ub_part_get_rbx(p, pos, rot);
    *size = ub_part_get_rbx_size(p);
    return true;
}

static i32 adornee_of(lctx *c, i32 inst)
{
    i32 a = pref(c->w, inst, c->g->k.Adornee);
    if (a >= 0) return a;
    i32 p = c->w->inst[inst].parent;
    return p >= 0 && c->w->inst[p].part >= 0 ? p : -1;
}

/* Where a world layer is and how big its canvas is; false if it is not
 * drawn this frame. */
static bool place_world_layer(lctx *c, glayer *l, const ub_camera *cam)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    i32 ad = adornee_of(c, l->inst);
    vec3 pos, size;
    mat3 rot;
    if (ad < 0 || !part_pose(w, ad, &pos, &rot, &size)) return false;
    l->quad.on_top = pbool(w, l->inst, K->AlwaysOnTop, false);
    l->quad.brightness = ub_clampf(pnum(w, l->inst, K->Brightness, 1), 0, 10);
    if (l->kind == K_SURFACE) {
        int face = pint(w, l->inst, K->Face, 5);
        /* face normal, and the GUI's right and down directions, in the part's axes */
        static const f32 nrm[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } };
        static const f32 rgt[6][3] = { { 0, 0, -1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, { -1, 0, 0 } };
        static const f32 dwn[6][3] = { { 0, -1, 0 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, -1 }, { 0, -1, 0 } };
        if (face < 0 || face > 5) face = 5;
        vec3 n = v3(nrm[face][0], nrm[face][1], nrm[face][2]), r = v3(rgt[face][0], rgt[face][1], rgt[face][2]),
             d = v3(dwn[face][0], dwn[face][1], dwn[face][2]);
        f32 half_n = ub_absf(v3_dot(n, size)) * 0.5f, fw = ub_absf(v3_dot(r, size)), fh = ub_absf(v3_dot(d, size));
        f32 zoff = fin(pnum(w, l->inst, K->ZOffset, 0), 1000) * 0.01f + 0.01f;
        vec3 centre = v3_add(v3_scale(n, half_n + zoff), v3(0, 0, 0));
        vec3 lc[4] = {
            v3_add(centre, v3_add(v3_scale(r, -fw * 0.5f), v3_scale(d, -fh * 0.5f))),
            v3_add(centre, v3_add(v3_scale(r, fw * 0.5f), v3_scale(d, -fh * 0.5f))),
            v3_add(centre, v3_add(v3_scale(r, fw * 0.5f), v3_scale(d, fh * 0.5f))),
            v3_add(centre, v3_add(v3_scale(r, -fw * 0.5f), v3_scale(d, fh * 0.5f))),
        };
        for (int k = 0; k < 4; ++k) {
            vec3 wp = v3_add(pos, mat3_apply(&rot, lc[k])); /* Roblox world */
            l->quad.corners[k] = ub_rbx_vec(wp);              /* Uniblox world */
        }
        int mode = pint(w, l->inst, K->SizingMode, 0);
        f32 cwid, chgt;
        if (mode == 1) {
            f32 pps = ub_clampf(pnum(w, l->inst, K->PixelsPerStud, 50), 1, 1000);
            cwid = fw * pps;
            chgt = fh * pps;
        } else {
            pv2(w, l->inst, K->CanvasSize, &cwid, &chgt, 800, 600);
        }
        l->w = ub_clampf(cwid, 1, 2048);
        l->h = ub_clampf(chgt, 1, 2048);
        return true;
    }
    /* BillboardGui */
    if (!cam) return false;
    vec3 off = pv3(w, l->inst, K->StudsOffsetWorldSpace);
    vec3 eows = pv3(w, l->inst, K->ExtentsOffsetWorldSpace);
    vec3 anchor = v3_add(pos, v3_add(off, v3(eows.x * size.x * 0.5f, eows.y * size.y * 0.5f, eows.z * size.z * 0.5f)));
    vec3 wa = ub_rbx_vec(anchor);
    /* camera-relative offsets (studs): right, up, towards the camera */
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw), sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    vec3 so = pv3(w, l->inst, K->StudsOffset), eo = pv3(w, l->inst, K->ExtentsOffset);
    f32 ext = v3_length(size) * 0.5f;
    so = v3_add(so, v3_scale(eo, ext));
    wa = v3_add(wa, v3_add(v3_add(v3_scale(right, so.x), v3_scale(up, so.y)), v3_scale(fwd, -so.z)));
    f32 px, py, ppu, depth;
    if (!project(cam, c->g->sw, c->g->sh, wa, &px, &py, &ppu, &depth)) return false;
    f32 maxd = pnum(w, l->inst, K->MaxDistance, 1e30f);
    if (maxd > 0 && depth > maxd) return false;
    udim2 z = { 0, 0, 0, 0 };
    udim2 s = pud2(w, l->inst, K->Size, z);
    f32 W = s.sx * ppu + s.ox, H = s.sy * ppu + s.oy;
    if (!(W >= 1) || !(H >= 1)) return false;
    W = ub_clampf(W, 1, 2048);
    H = ub_clampf(H, 1, 2048);
    f32 sox, soy;
    pv2(w, l->inst, K->SizeOffset, &sox, &soy, 0, 0);
    /* world corners of a rectangle facing the camera at the anchor's depth */
    f32 hw = W * 0.5f / ppu, hh = H * 0.5f / ppu;
    vec3 ctr = v3_add(wa, v3_add(v3_scale(right, sox * W / ppu), v3_scale(up, soy * H / ppu)));
    l->quad.corners[0] = v3_add(ctr, v3_add(v3_scale(right, -hw), v3_scale(up, hh)));
    l->quad.corners[1] = v3_add(ctr, v3_add(v3_scale(right, hw), v3_scale(up, hh)));
    l->quad.corners[2] = v3_add(ctr, v3_add(v3_scale(right, hw), v3_scale(up, -hh)));
    l->quad.corners[3] = v3_add(ctr, v3_add(v3_scale(right, -hw), v3_scale(up, -hh)));
    l->w = W;
    l->h = H;
    return true;
}

static int cmp_layers(const glayer *a, const glayer *b)
{
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return a->inst < b->inst ? -1 : a->inst > b->inst;
}

static void lay_out_layer(ub_gui *g, ub_world *w, glayer *l)
{
    lctx c = { g, w, l };
    const gkeys *K = &g->k;
    l->n = 0;
    l->ntexts = 0;
    l->sibling = pint(w, l->inst, K->ZIndexBehavior, 0) == 1;
    f32 ox = 0, oy = 0;
    if (l->kind == K_SCREEN) {
        bool ignore = pbool(w, l->inst, K->IgnoreGuiInset, false);
        l->w = (f32)g->sw;
        l->h = (f32)g->sh - (ignore ? 0 : UB_GUI_INSET);
        l->oy = ignore ? 0 : UB_GUI_INSET;
        oy = ignore ? -(f32)UB_GUI_INSET : 0; /* GUI coordinates start below the inset */
        l->order = pint(w, l->inst, K->DisplayOrder, 0);
    }
    gmem *lm = mem_of(g, w, l->inst);
    if (lm) {
        lm->gen = g->gen;
        lm->x = ox;
        lm->y = oy;
        lm->w = l->w;
        lm->h = l->h;
    }
    bool vis = pbool(w, l->inst, K->Enabled, true);
    ub_xform m = ub_xform_place(0, 0);
    int clip[4] = { -1000000, -1000000, 1000000, 1000000 };
    if (l->kind != K_SCREEN) {
        clip[0] = clip[1] = 0;
        clip[2] = (int)l->w;
        clip[3] = (int)l->h;
    }
    lay_children(&c, l->inst, -1, ox, oy, l->w, l->h, &m, 0, 1, clip, vis, true, NULL, NULL);
    /* drawing order */
    plat_free(l->draw);
    l->draw = (int *)plat_alloc(sizeof(int) * (size_t)(l->n + 1));
    if (l->draw) {
        for (int i = 0; i < l->n; ++i) l->draw[i] = i;
        if (!l->sibling) /* Global: stable by ZIndex (ties keep tree order: children over parents) */
            for (int i = 1; i < l->n; ++i) {
                int x = l->draw[i], j = i - 1;
                while (j >= 0 && l->e[l->draw[j]].z > l->e[x].z) {
                    l->draw[j + 1] = l->draw[j];
                    --j;
                }
                l->draw[j + 1] = x;
            }
    }
    l->laid_out = true;
    l->laid_w = l->w;
    l->laid_h = l->h;
    l->laid_gen = g->gen;
    l->dirty = true;
    if (l->kind == K_SCREEN) g->scr_dirty = true;
    g->stats.objects += (u32)l->n;
}

/* Reports properties the layout works out that changed (AbsoluteSize...). */
static void report_changes(ub_gui *g, ub_world *w)
{
    if (g->mem_cap > g->prev_cap && !grow((void **)&g->prev, &g->prev_cap, g->mem_cap, sizeof(gmem))) return;
    const gkeys *K = &g->k;
    for (i32 i = 0; i < g->mem_cap && i < w->count; ++i) {
        gmem *m = &g->mem[i], *p = &g->prev[i];
        if (m->gen != g->gen) continue;
        if (!p->known) {
            *p = *m;
            p->known = true;
            continue; /* the first layout is not a change */
        }
        if (m->w != p->w || m->h != p->h) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->AbsoluteSize);
        if (m->x != p->x || m->y != p->y) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->AbsolutePosition);
        if (m->rot != p->rot) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->AbsoluteRotation);
        if (m->tw != p->tw || m->th != p->th) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->TextBounds);
        if (m->fits != p->fits) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->TextFits);
        if (m->cw != p->cw || m->ch != p->ch) {
            u8 k = m->kind;
            event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, k == K_SCROLL ? K->AbsoluteCanvasSize : K->AbsoluteContentSize);
        }
        if (m->winw != p->winw || m->winh != p->winh) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->AbsoluteWindowSize);
        if (m->page != p->page) event(g, i, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, K->CurrentPage);
        *p = *m;
        p->known = true;
    }
}

static void lay_out_all(ub_gui *g, ub_world *w, const ub_camera *cam)
{
    u64 t0 = plat_time_us();
    resolve_keys(g, w);
    bool world_changed = w->epoch != g->epoch || g->need_layout;
    g->stats.objects = 0;
    if (world_changed) {
        collect_layers(g, w);
        g->gen++;
        g->nclamp = 0;
        for (int i = 0; i < g->nlayers; ++i) g->layers[i].laid_out = false;
    }
    g->nquads = 0;
    for (int i = 0; i < g->nlayers; ++i) {
        glayer *l = &g->layers[i];
        lctx c = { g, w, l };
        if (l->kind != K_SCREEN) {
            l->visible = pbool(w, l->inst, g->k.Enabled, true) && place_world_layer(&c, l, cam);
            if (!l->visible) continue;
            /* billboards resize as the camera moves: lay out again only then */
            if (l->laid_out && (ub_absf(l->laid_w - l->w) > 0.5f || ub_absf(l->laid_h - l->h) > 0.5f)) l->laid_out = false;
        }
        if (!l->laid_out) lay_out_layer(g, w, l);
    }
    /* screen layers by DisplayOrder */
    for (int i = 1; i < g->nlayers; ++i) {
        glayer x = g->layers[i];
        int j = i - 1;
        while (j >= 0 && cmp_layers(&g->layers[j], &x) > 0) {
            g->layers[j + 1] = g->layers[j];
            --j;
        }
        g->layers[j + 1] = x;
    }
    if (world_changed) {
        report_changes(g, w);
        /* CanvasPositions past the end are pulled back, as Roblox does */
        for (int i = 0; i < g->nclamp; ++i) {
            ub_value v;
            v.type = UB_PV_VECTOR2;
            v.v.v2.x = g->clamp_x[i];
            v.v.v2.y = g->clamp_y[i];
            if (ub_world_alive(w, g->clamp_inst[i])) {
                ub_world_set_prop(w, g->clamp_inst[i], g->k.CanvasPosition, &v);
                event(g, g->clamp_inst[i], UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.CanvasPosition);
            }
        }
        g->nclamp = 0;
        g->epoch = w->epoch;
        g->need_layout = false;
    }
    g->stats.layers = (u32)g->nlayers;
    g->stats.layout_us = (u32)(plat_time_us() - t0);
}

/* ------------------------------------------------------------- drawing */

static bool is_hovered(const ub_gui *g, i32 inst)
{
    for (int i = 0; i < g->nhover; ++i)
        if (g->hover[i] == inst) return true;
    return false;
}

static u32 shade(u32 c, f32 k)
{
    u32 r = (u32)(((c >> 16) & 255) * k), gg = (u32)(((c >> 8) & 255) * k), b = (u32)((c & 255) * k);
    return (UB_MIN(r, 255u) << 16) | (UB_MIN(gg, 255u) << 8) | UB_MIN(b, 255u);
}

/* UIGradient as a paint (sequences copied into `keys`) */
static void gradient_paint(lctx *c, i32 grad, ub_paint *p, f32 *ck, f32 *tk)
{
    p->gcol_n = p->gtr_n = 0;
    if (grad < 0) return;
    const gkeys *K = &c->g->k;
    const ub_value *col = P(c->w, grad, K->Color), *tr = P(c->w, grad, K->Transparency);
    if (col && col->type == UB_PV_COLORSEQ && col->v.seq.n <= UB_SEQ_MAX_KEYS) {
        memcpy(ck, col->v.seq.k, sizeof(f32) * col->v.seq.n * 4);
        p->gcol = ck;
        p->gcol_n = (int)col->v.seq.n;
    }
    if (tr && tr->type == UB_PV_NUMSEQ && tr->v.seq.n <= UB_SEQ_MAX_KEYS) {
        memcpy(tk, tr->v.seq.k, sizeof(f32) * tr->v.seq.n * 3);
        p->gtr = tk;
        p->gtr_n = (int)tr->v.seq.n;
    }
    f32 rot = pnum(c->w, grad, K->Rotation, 0) * (UB_PI / 180.0f);
    p->gdx = ub_cosf(rot);
    p->gdy = ub_sinf(rot);
    pv2(c->w, grad, K->Offset, &p->gox, &p->goy, 0, 0);
}

static void draw_image(lctx *c, ub_canvas *cv, const gelem *e, const ub_xform *t, f32 radius, const ub_paint *grad)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    u32 len;
    const char *img = pstr(w, e->inst, K->Image, &len);
    if (e->kind == K_IMAGEBUTTON) {
        const char *alt = NULL;
        u32 al;
        if (c->g->pressed[0] == e->inst) alt = pstr(w, e->inst, K->PressedImage, &al);
        else if (is_hovered(c->g, e->inst)) alt = pstr(w, e->inst, K->HoverImage, &al);
        if (alt && al) img = alt;
    }
    const ub_image_px *im = ub_image_get(img);
    if (!im) return;
    f32 rox, roy, rsx, rsy;
    pv2(w, e->inst, K->ImageRectOffset, &rox, &roy, 0, 0);
    pv2(w, e->inst, K->ImageRectSize, &rsx, &rsy, 0, 0);
    if (rsx <= 0 || rsy <= 0) {
        rsx = (f32)im->w - rox;
        rsy = (f32)im->h - roy;
    }
    f32 s0x = ub_clampf(rox, 0, (f32)im->w), s0y = ub_clampf(roy, 0, (f32)im->h);
    f32 s1x = ub_clampf(rox + rsx, 0, (f32)im->w), s1y = ub_clampf(roy + rsy, 0, (f32)im->h);
    if (s1x <= s0x || s1y <= s0y) return;
    int st = pint(w, e->inst, K->ScaleType, 0);
    ub_axis_map mx, my;
    f32 bw = e->w, bh = e->h;
    ub_xform bt = *t;
    if (st == 1) { /* Slice */
        const ub_value *sc = P(w, e->inst, K->SliceCenter);
        f32 k = ub_clampf(pnum(w, e->inst, K->SliceScale, 1), 0.01f, 100) * e->scale;
        f32 l0 = 0, t0 = 0, r0 = 0, b0 = 0;
        if (sc && sc->type == UB_PV_RECT) {
            l0 = sc->v.rect.x0;
            t0 = sc->v.rect.y0;
            r0 = sc->v.rect.x1;
            b0 = sc->v.rect.y1;
        }
        ub_axis_slice(&mx, bw, s0x, s1x, s0x + ub_clampf(l0, 0, s1x - s0x), s0x + ub_clampf(r0, 0, s1x - s0x), k);
        ub_axis_slice(&my, bh, s0y, s1y, s0y + ub_clampf(t0, 0, s1y - s0y), s0y + ub_clampf(b0, 0, s1y - s0y), k);
    } else if (st == 2) { /* Tile */
        udim2 d = { 1, 0, 1, 0 };
        udim2 ts = pud2(w, e->inst, K->TileSize, d);
        ub_axis_tile(&mx, bw, s0x, s1x, ts.sx * bw + ts.ox * e->scale);
        ub_axis_tile(&my, bh, s0y, s1y, ts.sy * bh + ts.oy * e->scale);
    } else if (st == 3 || st == 4) { /* Fit, Crop */
        f32 ia = (s1x - s0x) / (s1y - s0y), ba = bw / (bh > 0 ? bh : 1);
        if (st == 3) {
            f32 nw = ba > ia ? bh * ia : bw, nh = ba > ia ? bh : bw / ia;
            ub_xform off = ub_xform_place((bw - nw) * 0.5f, (bh - nh) * 0.5f);
            bt = ub_xform_mul(t, &off);
            bw = nw;
            bh = nh;
            ub_axis_stretch(&mx, bw, s0x, s1x);
            ub_axis_stretch(&my, bh, s0y, s1y);
        } else {
            f32 sw = s1x - s0x, sh = s1y - s0y;
            if (ba > ia) { /* box wider: crop top and bottom */
                f32 nh = sw / ba;
                ub_axis_stretch(&mx, bw, s0x, s1x);
                ub_axis_stretch(&my, bh, s0y + (sh - nh) * 0.5f, s0y + (sh + nh) * 0.5f);
            } else {
                f32 nw = sh * ba;
                ub_axis_stretch(&mx, bw, s0x + (sw - nw) * 0.5f, s0x + (sw + nw) * 0.5f);
                ub_axis_stretch(&my, bh, s0y, s1y);
            }
        }
    } else {
        ub_axis_stretch(&mx, bw, s0x, s1x);
        ub_axis_stretch(&my, bh, s0y, s1y);
    }
    ub_paint p = *grad;
    p.color = pcolor(w, e->inst, K->ImageColor3, 0xFFFFFF);
    p.alpha = 1 - ub_clampf(pnum(w, e->inst, K->ImageTransparency, 0), 0, 1);
    bool nearest = pint(w, e->inst, K->ResampleMode, 0) == 1;
    ub_canvas_image(cv, &bt, bw, bh, radius, im, &mx, &my, nearest, &p);
}

static void draw_elem(lctx *c, ub_canvas *cv, const gelem *e, f32 dx, f32 dy)
{
    const ub_world *w = c->w;
    const gkeys *K = &c->g->k;
    ub_gui *g = c->g;
    ub_canvas_clip(cv, e->cx0 + (int)dx, e->cy0 + (int)dy, e->cx1 + (int)dx, e->cy1 + (int)dy);
    /* the element's box on the canvas: whole pixels when unrotated */
    ub_xform shift = ub_xform_place(dx, dy);
    ub_xform m = ub_xform_mul(&shift, &e->m);
    f32 x = e->x, y = e->y, bw = e->w, bh = e->h;
    if (ub_xform_is_plain(&m)) {
        f32 X0 = (f32)ub_roundf(m.tx + x), Y0 = (f32)ub_roundf(m.ty + y);
        bw = (f32)ub_roundf(m.tx + x + e->w) - X0;
        bh = (f32)ub_roundf(m.ty + y + e->h) - Y0;
        x = X0 - m.tx;
        y = Y0 - m.ty;
    }
    ub_xform off = ub_xform_place(x, y);
    ub_xform t = ub_xform_mul(&m, &off);
    f32 radius = cornerpx(c, e->corner, bw, bh, e->scale);
    f32 ck[UB_SEQ_MAX_KEYS * 4], tk[UB_SEQ_MAX_KEYS * 3];
    ub_paint grad;
    memset(&grad, 0, sizeof grad);
    gradient_paint(c, e->gradient, &grad, ck, tk);
    /* background */
    f32 bt = ub_clampf(pnum(w, e->inst, K->BackgroundTransparency, 0), 0, 1);
    if (bt < 1) {
        ub_paint p = grad;
        p.color = pcolor(w, e->inst, K->BackgroundColor3, 0xA3A2A5);
        if (IS_BUTTON(e->kind) && pbool(w, e->inst, K->AutoButtonColor, true)) {
            if (g->pressed[0] == e->inst && is_hovered(g, e->inst)) p.color = shade(p.color, 0.6f);
            else if (is_hovered(g, e->inst)) p.color = shade(p.color, 0.8f);
        }
        p.alpha = 1 - bt;
        if (e->kind != K_VIDEO) ub_canvas_rect(cv, &t, bw, bh, radius, &p);
        /* the border (none with rounded corners, as in Roblox) */
        int bs = pint(w, e->inst, K->BorderSizePixel, 1);
        if (bs > 0 && e->corner < 0) {
            ub_paint bp;
            memset(&bp, 0, sizeof bp);
            bp.color = pcolor(w, e->inst, K->BorderColor3, 0x1B2A35);
            bp.alpha = 1 - bt;
            int mode = pint(w, e->inst, K->BorderMode, 0);
            f32 b = (f32)bs * (e->scale > 0 ? e->scale : 1);
            if (b < 1) b = 1;
            f32 in = mode == 2 ? b : mode == 1 ? b * 0.5f : 0, out = mode == 0 ? b : mode == 1 ? b * 0.5f : 0;
            ub_canvas_ring(cv, &t, bw, bh, 0, in, out, &bp);
        }
    }
    if (IS_IMAGE(e->kind)) draw_image(c, cv, e, &t, radius, &grad);
    if (e->kind == K_VIEWPORT) {
        /* 3D inside the GUI is not drawn yet: a faint tint marks where it is */
    }
    /* text */
    ub_text_stroke stroke;
    stroke.color = 0;
    stroke.transparency = 1;
    stroke.thickness = 0;
    bool text_stroke = false;
    if (e->stroke >= 0 && pint(w, e->stroke, K->ApplyStrokeMode, 0) == 0 && IS_TEXT(e->kind)) text_stroke = true;
    if (IS_TEXT(e->kind) && e->text >= 0) {
        stroke.color = pcolor(w, e->inst, K->TextStrokeColor3, 0);
        stroke.transparency = ub_clampf(pnum(w, e->inst, K->TextStrokeTransparency, 1), 0, 1);
        if (text_stroke) {
            stroke.color = pcolor(w, e->stroke, K->Color, 0);
            stroke.transparency = ub_clampf(pnum(w, e->stroke, K->Transparency, 0), 0, 1);
            stroke.thickness = ub_clampf(pnum(w, e->stroke, K->Thickness, 1), 0, 50) * e->scale;
        }
        ub_xform tp = ub_xform_place(e->pad[2], e->pad[0]);
        ub_xform tt = ub_xform_mul(&t, &tp);
        const ub_text_layout *tl = &c->l->texts[e->text];
        f32 tw = bw - e->pad[2] - e->pad[3], th = bh - e->pad[0] - e->pad[1];
        int maxg = pint(w, e->inst, K->MaxVisibleGraphemes, -1);
        ub_text_draw(cv, &tt, tl, tw, th, grad.gcol_n || grad.gtr_n ? &grad : NULL, maxg, &stroke, 1);
        /* the caret */
        if (e->kind == K_TEXTBOX && g->focus == e->inst && g->caret_on) {
            u32 len;
            pstr(w, e->inst, K->Text, &len);
            int cur = pint(w, e->inst, K->CursorPosition, (int)len + 1) - 1;
            f32 cx = 0, cy = 0, size = tl->nruns ? tl->runs[0].size : 14;
            if (len == 0) { /* empty: where the placeholder starts */
                int xa = pint(w, e->inst, K->TextXAlignment, 2), ya = pint(w, e->inst, K->TextYAlignment, 1);
                cx = xa == 0 ? 0 : xa == 1 ? tw : tw * 0.5f;
                cy = ya == 0 ? size * 0.8f : ya == 2 ? th - size * 0.2f : th * 0.5f + size * 0.3f;
            } else {
                /* glyphs are one per code point: count code points before the cursor */
                const char *s = pstr(w, e->inst, K->Text, &len), *q = s;
                int gi = 0;
                while (q < s + len && q < s + (cur < 0 ? 0 : cur)) {
                    ub_utf8_decode(&q);
                    ++gi;
                }
                if (gi < tl->n) {
                    cx = tl->g[gi].x;
                    cy = tl->g[gi].y;
                } else if (tl->n > 0) {
                    cx = tl->g[tl->n - 1].x + tl->g[tl->n - 1].adv;
                    cy = tl->g[tl->n - 1].y;
                }
            }
            ub_paint cp;
            memset(&cp, 0, sizeof cp);
            cp.color = pcolor(w, e->inst, K->TextColor3, 0x1B2A35);
            cp.alpha = 1;
            ub_xform at = ub_xform_place(cx, cy - size * 0.8f);
            ub_xform ct = ub_xform_mul(&tt, &at);
            ub_canvas_rect(cv, &ct, size > 24 ? 2 : 1, size, 0, &cp);
        }
    }
    /* UIStroke as a border */
    if (e->stroke >= 0 && !text_stroke) {
        ub_paint sp;
        memset(&sp, 0, sizeof sp);
        sp.color = pcolor(w, e->stroke, K->Color, 0);
        sp.alpha = 1 - ub_clampf(pnum(w, e->stroke, K->Transparency, 0), 0, 1);
        f32 th = ub_clampf(pnum(w, e->stroke, K->Thickness, 1), 0, 100) * e->scale;
        if (th > 0) ub_canvas_ring(cv, &t, bw, bh, radius, 0, th, &sp);
    }
}

static void draw_scrollbars(lctx *c, ub_canvas *cv, const gelem *e, f32 dx, f32 dy)
{
    const gkeys *K = &c->g->k;
    const ub_world *w = c->w;
    if (!e->bar_v && !e->bar_h) return;
    if (!pbool(w, e->inst, K->ScrollingEnabled, true)) return;
    f32 th = ub_clampf((f32)pint(w, e->inst, K->ScrollBarThickness, 12), 0, 200) * e->scale;
    if (th <= 0) return;
    ub_paint p;
    memset(&p, 0, sizeof p);
    p.color = pcolor(w, e->inst, K->ScrollBarImageColor3, 0xFFFFFF);
    p.alpha = 1 - ub_clampf(pnum(w, e->inst, K->ScrollBarImageTransparency, 0), 0, 1);
    ub_canvas_clip(cv, e->cx0 + (int)dx, e->cy0 + (int)dy, e->cx1 + (int)dx, e->cy1 + (int)dy);
    ub_xform shift = ub_xform_place(dx, dy);
    ub_xform m = ub_xform_mul(&shift, &e->m);
    if (e->bar_v) {
        f32 len = e->h * e->h / e->canvas_h, room = e->h - len;
        if (len < th * 2) len = th * 2;
        room = e->h - len;
        f32 maxy = e->canvas_h - e->h;
        f32 at = maxy > 0 ? e->cpy / maxy * room : 0;
        bool left = pint(w, e->inst, K->VerticalScrollBarPosition, 0) == 1;
        ub_xform o = ub_xform_place(e->x + (left ? 0 : e->w - th), e->y + at);
        ub_xform t = ub_xform_mul(&m, &o);
        ub_canvas_rect(cv, &t, th, len, th * 0.5f, &p);
    }
    if (e->bar_h) {
        f32 len = e->w * e->w / e->canvas_w;
        if (len < th * 2) len = th * 2;
        f32 room = e->w - len, maxx = e->canvas_w - e->w;
        f32 at = maxx > 0 ? e->cpx / maxx * room : 0;
        ub_xform o = ub_xform_place(e->x + at, e->y + e->h - th);
        ub_xform t = ub_xform_mul(&m, &o);
        ub_canvas_rect(cv, &t, len, th, th * 0.5f, &p);
    }
}

/* the element's bounding box on its layer's canvas */
static void elem_bounds(const gelem *e, int *x0, int *y0, int *x1, int *y1)
{
    f32 cx[4] = { e->x, e->x + e->w, e->x + e->w, e->x }, cy[4] = { e->y, e->y, e->y + e->h, e->y + e->h };
    f32 X0 = 1e9f, Y0 = 1e9f, X1 = -1e9f, Y1 = -1e9f;
    for (int k = 0; k < 4; ++k) {
        f32 X = e->m.a * cx[k] + e->m.c * cy[k] + e->m.tx, Y = e->m.b * cx[k] + e->m.d * cy[k] + e->m.ty;
        X0 = UB_MIN(X0, X);
        X1 = UB_MAX(X1, X);
        Y0 = UB_MIN(Y0, Y);
        Y1 = UB_MAX(Y1, Y);
    }
    /* room for strokes, borders and text that spills out */
    *x0 = ub_floorf(X0) - 64;
    *y0 = ub_floorf(Y0) - 64;
    *x1 = ub_ceilf(X1) + 64;
    *y1 = ub_ceilf(Y1) + 64;
}

static void draw_layer(ub_gui *g, ub_world *w, glayer *l, ub_canvas *cv, f32 dx, f32 dy, int *rect)
{
    lctx c = { g, w, l };
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    for (int q = 0; q < l->n; ++q) {
        const gelem *e = &l->e[l->draw ? l->draw[q] : q];
        if (!e->visible) continue;
        draw_elem(&c, cv, e, dx, dy);
        int x0, y0, x1, y1;
        elem_bounds(e, &x0, &y0, &x1, &y1);
        x0 = UB_MAX(x0, e->cx0);
        y0 = UB_MAX(y0, e->cy0);
        x1 = UB_MIN(x1, e->cx1);
        y1 = UB_MIN(y1, e->cy1);
        if (x0 < bx0) bx0 = x0;
        if (y0 < by0) by0 = y0;
        if (x1 > bx1) bx1 = x1;
        if (y1 > by1) by1 = y1;
    }
    for (int q = 0; q < l->n; ++q) {
        const gelem *e = &l->e[l->draw ? l->draw[q] : q];
        if (e->visible && e->kind == K_SCROLL) draw_scrollbars(&c, cv, e, dx, dy);
    }
    if (rect) {
        rect[0] = UB_MAX(bx0 + (int)dx, 0);
        rect[1] = UB_MAX(by0 + (int)dy, 0);
        rect[2] = UB_MIN(bx1 + (int)dx, cv->w);
        rect[3] = UB_MIN(by1 + (int)dy, cv->h);
    }
}

static void render_screen(ub_gui *g, ub_world *w)
{
    if (g->scr_w != g->sw || g->scr_h != g->sh || !g->screen) {
        plat_free(g->screen);
        g->screen = NULL;
        g->scr_w = g->scr_h = 0;
        g->nold = g->nrects = 0;
        if (g->sw > 0 && g->sh > 0) {
            g->screen = (u32 *)plat_alloc(sizeof(u32) * (size_t)g->sw * (size_t)g->sh);
            if (!g->screen) return;
            memset(g->screen, 0, sizeof(u32) * (size_t)g->sw * (size_t)g->sh);
            g->scr_w = g->sw;
            g->scr_h = g->sh;
        }
        g->scr_dirty = true;
    }
    if (!g->screen || !g->scr_dirty) return;
    ub_canvas cv;
    ub_canvas_init(&cv, g->screen, g->scr_w, g->scr_h, g->scr_w);
    /* clear what was drawn last time */
    for (int i = 0; i < g->nrects; ++i)
        ub_canvas_clear(&cv, g->rects[i * 4], g->rects[i * 4 + 1], g->rects[i * 4 + 2], g->rects[i * 4 + 3]);
    g->nrects = 0;
    for (int i = 0; i < g->nlayers; ++i) {
        glayer *l = &g->layers[i];
        if (l->kind != K_SCREEN || !l->n) continue;
        int r[4];
        draw_layer(g, w, l, &cv, 0, UB_GUI_INSET, r); /* GUI coordinates start below the inset */
        if (r[2] > r[0] && r[3] > r[1]) {
            if (g->nrects < 64) {
                memcpy(&g->rects[g->nrects * 4], r, sizeof r);
                g->nrects++;
            } else { /* too many layers: grow the last one */
                int *q = &g->rects[63 * 4];
                q[0] = UB_MIN(q[0], r[0]);
                q[1] = UB_MIN(q[1], r[1]);
                q[2] = UB_MAX(q[2], r[2]);
                q[3] = UB_MAX(q[3], r[3]);
            }
        }
        l->dirty = false;
    }
    g->scr_version++;
    g->scr_dirty = false;
}

static void render_world_layer(ub_gui *g, ub_world *w, glayer *l)
{
    int W = (int)(l->w + 0.5f), H = (int)(l->h + 0.5f);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (l->pw != W || l->ph != H || !l->px) {
        plat_free(l->px);
        l->px = (u32 *)plat_alloc(sizeof(u32) * (size_t)W * (size_t)H);
        if (!l->px) {
            l->pw = l->ph = 0;
            return;
        }
        l->pw = W;
        l->ph = H;
        l->dirty = true;
    }
    if (l->dirty) {
        memset(l->px, 0, sizeof(u32) * (size_t)W * (size_t)H);
        ub_canvas cv;
        ub_canvas_init(&cv, l->px, W, H, W);
        draw_layer(g, w, l, &cv, 0, 0, NULL);
        l->dirty = false;
        l->version++;
    }
    l->quad.inst = l->inst;
    l->quad.image.px = l->px;
    l->quad.image.w = W;
    l->quad.image.h = H;
    l->quad.image.pitch = W;
    l->quad.image.version = l->version;
    if (grow((void **)&g->quads, &g->quads_cap, g->nquads + 1, sizeof(ub_gui_quad))) g->quads[g->nquads++] = l->quad;
}

void ub_gui_render(ub_gui *g, ub_world *w, const ub_camera *cam)
{
    u64 t0 = plat_time_us();
    if (cam) {
        g->cam = *cam;
        g->have_cam = true;
    }
    lay_out_all(g, w, g->have_cam ? &g->cam : NULL);
    /* hover and press changes are visual only */
    render_screen(g, w);
    for (int i = 0; i < g->nlayers; ++i) {
        glayer *l = &g->layers[i];
        if (l->kind != K_SCREEN && l->visible) render_world_layer(g, w, l);
    }
    g->stats.drawn_us = (u32)(plat_time_us() - t0);
}

const ub_render_image *ub_gui_screen(const ub_gui *g, const int **rects, int *nrects)
{
    static ub_render_image img;
    if (!g->screen || g->nrects == 0) {
        *rects = NULL;
        *nrects = 0;
        return NULL;
    }
    img.px = g->screen;
    img.w = g->scr_w;
    img.h = g->scr_h;
    img.pitch = g->scr_w;
    img.id = 0x6AFFFFFFu;
    img.version = g->scr_version;
    *rects = g->rects;
    *nrects = g->nrects;
    return &img;
}

int ub_gui_quads(const ub_gui *g, const ub_gui_quad **out)
{
    *out = g->quads;
    return g->nquads;
}

/* -------------------------------------------------------------- input */

/* Is a screen point (layer pixels) inside an element and its clip? */
static bool hit(const gelem *e, f32 X, f32 Y)
{
    if (X < (f32)e->cx0 || Y < (f32)e->cy0 || X >= (f32)e->cx1 || Y >= (f32)e->cy1) return false;
    f32 x, y;
    if (!ub_xform_inverse(&e->m, X, Y, &x, &y)) return false;
    return x >= e->x && y >= e->y && x < e->x + e->w && y < e->y + e->h;
}

/* GuiObjects under a point, bottom first (screen layers). */
static int collect_hits(ub_gui *g, f32 sx, f32 sy, i32 *out, bool *active, int max)
{
    int n = 0;
    for (int i = 0; i < g->nlayers; ++i) {
        glayer *l = &g->layers[i];
        if (l->kind != K_SCREEN) continue;
        f32 X = sx, Y = sy - UB_GUI_INSET; /* GUI coordinates, for both kinds of ScreenGui */
        for (int q = 0; q < l->n && n < max; ++q) {
            const gelem *e = &l->e[l->draw ? l->draw[q] : q];
            if (!e->visible || !hit(e, X, Y)) continue;
            out[n] = e->inst;
            if (active) active[n] = e->active;
            n++;
        }
    }
    return n;
}

static gelem *find_elem(ub_gui *g, i32 inst)
{
    for (int i = 0; i < g->nlayers; ++i) {
        glayer *l = &g->layers[i];
        for (int q = 0; q < l->n; ++q)
            if (l->e[q].inst == inst) return &l->e[q];
    }
    return NULL;
}

static void set_text(ub_gui *g, ub_world *w, i32 box, const char *s, u32 len, int cursor)
{
    ub_value v = ub_vstr(s, len);
    ub_world_set_prop(w, box, g->k.Text, &v);
    ub_value c = ub_vint(cursor);
    ub_world_set_prop(w, box, g->k.CursorPosition, &c);
    event(g, box, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.Text);
    event(g, box, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.CursorPosition);
}

void ub_gui_focus(ub_gui *g, ub_world *w, i32 box)
{
    resolve_keys(g, w);
    if (g->focus == box) return;
    if (g->focus >= 0) ub_gui_release_focus(g, w, false);
    if (!ub_world_alive(w, box) || kind_of(g, w, box) != K_TEXTBOX) return;
    g->focus = box;
    g->caret_on = true;
    g->caret_t0 = g->now;
    u32 len;
    const char *t = pstr(w, box, g->k.Text, &len);
    if (pbool(w, box, g->k.ClearTextOnFocus, true) && len) set_text(g, w, box, "", 0, 1);
    else {
        (void)t;
        ub_value c = ub_vint((int)len + 1);
        ub_world_set_prop(w, box, g->k.CursorPosition, &c);
    }
    event(g, box, UB_GEV_FOCUSED, 0, 0, 0, 0, 0);
    g->scr_dirty = true;
}

void ub_gui_release_focus(ub_gui *g, ub_world *w, bool enter)
{
    i32 box = g->focus;
    if (box < 0) return;
    g->focus = -1;
    resolve_keys(g, w);
    if (ub_world_alive(w, box)) {
        ub_value c = ub_vint(-1);
        ub_world_set_prop(w, box, g->k.CursorPosition, &c);
    }
    event(g, box, UB_GEV_FOCUS_LOST, 0, 0, 0, enter ? 1 : 0, 0);
    g->scr_dirty = true;
}

static void edit_text(ub_gui *g, ub_world *w, const ub_gui_input *in)
{
    i32 box = g->focus;
    if (box < 0) return;
    if (!ub_world_alive(w, box)) {
        g->focus = -1;
        return;
    }
    u32 len;
    const char *cur_text = pstr(w, box, g->k.Text, &len);
    char *buf = (char *)plat_alloc(len + 4096 + 8);
    if (!buf) return;
    memcpy(buf, cur_text, len);
    int cur = pint(w, box, g->k.CursorPosition, (int)len + 1) - 1;
    if (cur < 0 || cur > (int)len) cur = (int)len;
    bool editable = pbool(w, box, g->k.TextEditable, true), multi = pbool(w, box, g->k.MultiLine, false);
    bool changed = false, moved = false, done = false, enter = false;
    for (const char *p = in->text ? in->text : ""; *p && !done;) {
        const char *s0 = p;
        u32 ch = ub_utf8_decode(&p);
        if (ch == 0x1B) {
            done = true;
        } else if (ch == '\r' || ch == '\n') {
            if (multi && editable && len < (u32)(len + 4096)) {
                memmove(buf + cur + 1, buf + cur, len - (u32)cur);
                buf[cur++] = '\n';
                len++;
                changed = true;
            } else {
                done = enter = true;
            }
        } else if (ch == '\b') {
            if (editable && cur > 0) {
                int s = cur - 1;
                while (s > 0 && ((u8)buf[s] & 0xC0) == 0x80) --s;
                memmove(buf + s, buf + cur, len - (u32)cur);
                len -= (u32)(cur - s);
                cur = s;
                changed = true;
            }
        } else if (ch >= 32 && editable) {
            u32 n = (u32)(p - s0);
            if (len + n <= len + 4096 && len + n < 200000) {
                memmove(buf + cur + n, buf + cur, len - (u32)cur);
                memcpy(buf + cur, s0, n);
                cur += (int)n;
                len += n;
                changed = true;
            }
        }
    }
    if (in->key_pressed[UB_KEY_LEFT] && cur > 0) {
        --cur;
        while (cur > 0 && ((u8)buf[cur] & 0xC0) == 0x80) --cur;
        moved = true;
    }
    if (in->key_pressed[UB_KEY_RIGHT] && cur < (int)len) {
        ++cur;
        while (cur < (int)len && ((u8)buf[cur] & 0xC0) == 0x80) ++cur;
        moved = true;
    }
    if (in->key_pressed[UB_KEY_HOME]) { cur = 0; moved = true; }
    if (in->key_pressed[UB_KEY_END]) { cur = (int)len; moved = true; }
    if (in->key_pressed[UB_KEY_DELETE] && editable && cur < (int)len) {
        int e = cur + 1;
        while (e < (int)len && ((u8)buf[e] & 0xC0) == 0x80) ++e;
        memmove(buf + cur, buf + e, len - (u32)e);
        len -= (u32)(e - cur);
        changed = true;
    }
    if (changed) set_text(g, w, box, buf, len, cur + 1);
    else if (moved) {
        ub_value c = ub_vint(cur + 1);
        ub_world_set_prop(w, box, g->k.CursorPosition, &c);
        event(g, box, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.CursorPosition);
    }
    if (changed || moved) {
        g->caret_on = true;
        g->caret_t0 = g->now;
    }
    plat_free(buf);
    if (done) ub_gui_release_focus(g, w, enter);
}

static void scroll_by(ub_gui *g, ub_world *w, gelem *e, f32 dx, f32 dy)
{
    f32 cx, cy;
    pv2(w, e->inst, g->k.CanvasPosition, &cx, &cy, 0, 0);
    f32 maxx = e->canvas_w - e->w, maxy = e->canvas_h - e->h;
    f32 nx = ub_clampf(cx + dx, 0, maxx > 0 ? maxx : 0), ny = ub_clampf(cy + dy, 0, maxy > 0 ? maxy : 0);
    if (nx == cx && ny == cy) return;
    ub_value v;
    v.type = UB_PV_VECTOR2;
    v.v.v2.x = nx;
    v.v.v2.y = ny;
    ub_world_set_prop(w, e->inst, g->k.CanvasPosition, &v);
    event(g, e->inst, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.CanvasPosition);
}

void ub_gui_update(ub_gui *g, ub_world *w, int screen_w, int screen_h, const ub_camera *cam, const ub_gui_input *in,
                   f64 time)
{
    g->now = time;
    if (screen_w != g->sw || screen_h != g->sh) {
        g->sw = screen_w;
        g->sh = screen_h;
        g->need_layout = true;
    }
    if (cam) {
        g->cam = *cam;
        g->have_cam = true;
    }
    lay_out_all(g, w, g->have_cam ? &g->cam : NULL);
    g->mouse_taken = g->wheel_taken = false;
    if (!in) return;
    /* the caret blinks */
    if (g->focus >= 0) {
        bool on = ((int)((time - g->caret_t0) * 2.0)) % 2 == 0;
        if (on != g->caret_on) {
            g->caret_on = on;
            g->scr_dirty = true;
        }
    }
    f32 mx = in->mouse_x, my = in->mouse_y;
    i32 gx = (i32)mx, gy = (i32)(my - UB_GUI_INSET); /* GUI coordinates, as scripts see them */
    i32 hits[256];
    bool act[256];
    int nh = in->mouse_free ? collect_hits(g, mx, my, hits, act, 256) : 0;
    /* MouseEnter / MouseLeave / MouseMoved */
    bool moved = mx != g->mx || my != g->my;
    for (int i = 0; i < g->nhover; ++i) {
        bool still = false;
        for (int k = 0; k < nh && !still; ++k) still = hits[k] == g->hover[i];
        if (!still) {
            event(g, g->hover[i], UB_GEV_MOUSE_LEAVE, gx, gy, 0, 0, 0);
            if (IS_BUTTON(kind_of(g, w, g->hover[i]))) g->scr_dirty = true;
        }
    }
    for (int k = 0; k < nh; ++k) {
        if (!is_hovered(g, hits[k])) {
            event(g, hits[k], UB_GEV_MOUSE_ENTER, gx, gy, 0, 0, 0);
            if (IS_BUTTON(kind_of(g, w, hits[k]))) g->scr_dirty = true;
        } else if (moved) {
            event(g, hits[k], UB_GEV_MOUSE_MOVED, gx, gy, 0, 0, 0);
            event(g, hits[k], UB_GEV_INPUT_CHANGED, gx, gy, 3, 0, 0);
        }
    }
    if (grow((void **)&g->hover, &g->hover_cap, nh + 1, sizeof(i32))) {
        memcpy(g->hover, hits, sizeof(i32) * (size_t)nh);
        g->nhover = nh;
    }
    g->mx = mx;
    g->my = my;
    /* who gets a click: the topmost button or text box, unless an Active
     * object lies over it */
    i32 target = -1;
    bool taken = false;
    for (int k = nh - 1; k >= 0; --k) {
        u8 kk = kind_of(g, w, hits[k]);
        if (act[k]) taken = true;
        if (IS_BUTTON(kk) || kk == K_TEXTBOX) {
            target = hits[k];
            break;
        }
        if (act[k]) break;
    }
    g->mouse_taken = taken;
    /* scrollbar dragging */
    if (g->drag_scroll >= 0) {
        gelem *e = find_elem(g, g->drag_scroll);
        if (!e || !in->down[0]) g->drag_scroll = -1;
        else {
            f32 room = e->h - UB_MAX(e->h * e->h / e->canvas_h, 24.0f);
            f32 maxy = e->canvas_h - e->h;
            f32 want = room > 0 ? (my - UB_GUI_INSET - g->drag_off - e->y) / room * maxy : 0;
            f32 cx, cy;
            pv2(w, e->inst, g->k.CanvasPosition, &cx, &cy, 0, 0);
            scroll_by(g, w, e, 0, want - cy);
            g->mouse_taken = true;
        }
    }
    for (int b = 0; b < 2; ++b) {
        if (in->pressed[b]) {
            /* a press on a scrollbar starts dragging it */
            if (b == 0) {
                for (int k = nh - 1; k >= 0; --k) {
                    gelem *e = find_elem(g, hits[k]);
                    if (!e || e->kind != K_SCROLL || !e->bar_v) continue;
                    f32 th = (f32)pint(w, e->inst, g->k.ScrollBarThickness, 12) * e->scale;
                    if ((f32)gx >= e->x + e->w - th) {
                        f32 len = UB_MAX(e->h * e->h / e->canvas_h, 24.0f), maxy = e->canvas_h - e->h;
                        f32 at = maxy > 0 ? e->cpy / maxy * (e->h - len) : 0;
                        g->drag_scroll = e->inst;
                        g->drag_off = (f32)gy - e->y - at;
                        if (g->drag_off < 0 || g->drag_off > len) g->drag_off = len * 0.5f;
                        target = -1;
                        g->mouse_taken = true;
                    }
                    break;
                }
            }
            for (int k = nh - 1; k >= 0; --k) event(g, hits[k], UB_GEV_INPUT_BEGAN, gx, gy, (u8)b, 0, 0);
            if (target >= 0) {
                g->pressed[b] = target;
                event(g, target, b ? UB_GEV_B2_DOWN : UB_GEV_B1_DOWN, gx, gy, (u8)b, 0, 0);
                g->scr_dirty = true;
            }
            /* clicking a text box focuses it; clicking anything else lets go */
            if (b == 0) {
                if (target >= 0 && kind_of(g, w, target) == K_TEXTBOX) ub_gui_focus(g, w, target);
                else if (g->focus >= 0) ub_gui_release_focus(g, w, false);
            }
        }
        if (in->released[b]) {
            for (int k = nh - 1; k >= 0; --k) event(g, hits[k], UB_GEV_INPUT_ENDED, gx, gy, (u8)b, 0, 0);
            i32 p = g->pressed[b];
            if (p >= 0) {
                event(g, p, b ? UB_GEV_B2_UP : UB_GEV_B1_UP, gx, gy, (u8)b, 0, 0);
                if (p == target) {
                    event(g, p, b ? UB_GEV_B2_CLICK : UB_GEV_B1_CLICK, gx, gy, (u8)b, 0, 0);
                    if (b == 0 && IS_BUTTON(kind_of(g, w, p))) {
                        if (p == g->last_click_inst && time - g->last_click_t < 0.5) g->click_count++;
                        else g->click_count = 1;
                        g->last_click_inst = p;
                        g->last_click_t = time;
                        event(g, p, UB_GEV_ACTIVATED, gx, gy, 0, (u8)UB_MIN(g->click_count, 255), 0);
                    }
                }
                g->pressed[b] = -1;
                g->scr_dirty = true;
            }
        }
    }
    /* the wheel scrolls the innermost ScrollingFrame under the mouse */
    if (in->wheel) {
        for (int k = nh - 1; k >= 0; --k) event(g, hits[k], in->wheel > 0 ? UB_GEV_WHEEL_FORWARD : UB_GEV_WHEEL_BACKWARD, gx, gy, 4, 0, 0);
        for (int k = nh - 1; k >= 0; --k) {
            gelem *e = find_elem(g, hits[k]);
            if (!e || e->kind != K_SCROLL || !pbool(w, e->inst, g->k.ScrollingEnabled, true)) continue;
            if (e->bar_v) scroll_by(g, w, e, 0, (f32)-in->wheel * 48.0f * e->scale);
            else if (e->bar_h) scroll_by(g, w, e, (f32)-in->wheel * 48.0f * e->scale, 0);
            else continue;
            g->wheel_taken = true;
            break;
        }
    }
    edit_text(g, w, in);
}

bool ub_gui_mouse_taken(const ub_gui *g) { return g->mouse_taken; }
bool ub_gui_wheel_taken(const ub_gui *g) { return g->wheel_taken; }
bool ub_gui_keyboard_taken(const ub_gui *g) { return g->focus >= 0; }
i32 ub_gui_focused_box(const ub_gui *g) { return g->focus; }

/* -------------------------------------------------------------- queries */

bool ub_gui_info_of(ub_gui *g, ub_world *w, i32 inst, ub_gui_info *out)
{
    memset(out, 0, sizeof *out);
    out->page = -1;
    if (w->epoch != g->epoch || g->need_layout) lay_out_all(g, w, g->have_cam ? &g->cam : NULL);
    gmem *m = mem_of(g, w, inst);
    if (!m || m->gen == 0) return false;
    out->x = m->x;
    out->y = m->y;
    out->w = m->w;
    out->h = m->h;
    out->rotation = m->rot;
    out->text_w = m->tw;
    out->text_h = m->th;
    out->text_fits = m->fits;
    out->content_w = m->cw;
    out->content_h = m->ch;
    out->canvas_w = m->cw;
    out->canvas_h = m->ch;
    out->window_w = m->winw;
    out->window_h = m->winh;
    out->cell_w = m->cellw;
    out->cell_h = m->cellh;
    out->cells_x = m->cellsx;
    out->cells_y = m->cellsy;
    out->page = m->page;
    return m->gen == g->gen || m->gen > 0;
}

void ub_gui_screen_size(const ub_gui *g, int *w, int *h)
{
    *w = g->sw;
    *h = g->sh;
}

void ub_gui_page_jump(ub_gui *g, ub_world *w, i32 layout, i32 page)
{
    gmem *m = mem_of(g, w, layout);
    if (!m) return;
    if (m->page != page) {
        m->page = page;
        g->need_layout = true;
        event(g, layout, UB_GEV_PROP_CHANGED, 0, 0, 0, 0, g->k.CurrentPage);
    }
}

void ub_gui_page_step(ub_gui *g, ub_world *w, i32 layout, int step)
{
    gmem *m = mem_of(g, w, layout);
    if (!m || !ub_world_alive(w, layout)) return;
    i32 parent = w->inst[layout].parent;
    if (parent < 0) return;
    lctx c = { g, w, NULL };
    kids k;
    get_kids(&c, parent, &k);
    if (!k.n) return;
    sort_for_layout(&c, layout, k.objs, k.n);
    int cur = 0;
    for (int i = 0; i < k.n; ++i)
        if (k.objs[i] == m->page) cur = i;
    int next = cur + step;
    if (pbool(w, layout, g->k.Circular, false)) next = ((next % k.n) + k.n) % k.n;
    if (next < 0 || next >= k.n) return;
    ub_gui_page_jump(g, w, layout, k.objs[next]);
}

int ub_gui_objects_at(ub_gui *g, ub_world *w, f32 x, f32 y, i32 *out, int max)
{
    if (w->epoch != g->epoch || g->need_layout) lay_out_all(g, w, g->have_cam ? &g->cam : NULL);
    i32 hits[256];
    int n = collect_hits(g, x, y + UB_GUI_INSET, hits, NULL, 256);
    int k = 0;
    for (int i = n - 1; i >= 0 && k < max; --i) out[k++] = hits[i];
    return k;
}

void ub_gui_get_stats(const ub_gui *g, ub_gui_stats *out) { *out = g->stats; }
