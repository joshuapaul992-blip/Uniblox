/* Uniblox - text for the GUI: choosing a typeface for Roblox's fonts,
 * caching glyphs, rich text, line breaking, alignment, TextScaled and
 * drawing.
 *
 * Sizes follow Roblox: TextSize is the height of a line in pixels, so a
 * single line's TextBounds.Y equals TextSize (times LineHeight). */
#ifndef UB_TEXT_H
#define UB_TEXT_H

#include "gui/ub_canvas.h"
#include "platform/platform.h"

/* A Roblox font: Enum.Font value, or a FontFace (family content id,
 * weight 100..900, style 0 normal / 1 italic). */
ub_typeface ub_text_face_enum(int font);
ub_typeface ub_text_face_family(const char *family, int weight, int style);

typedef struct {
    ub_typeface face;
    f32 size;               /* TextSize */
    u32 color;              /* 0x00RRGGBB */
    f32 transparency;
    u8 underline, strike;
    u8 stroke;              /* has a stroke (rich text <stroke>) */
    u32 stroke_color;
    f32 stroke_transparency;
    f32 stroke_thickness;
} ub_trun;

typedef struct {
    u32 cp;
    u16 run;
    f32 x, y;               /* pen position on the baseline, in the box */
    f32 adv;
    i32 index;              /* grapheme index (MaxVisibleGraphemes) */
} ub_tglyph;

typedef struct {
    /* what to lay out */
    const char *text;
    u32 len;
    bool rich;              /* RichText */
    ub_trun base;           /* the label's own font, size and colours */
    f32 line_height;        /* LineHeight (1 = normal) */
    f32 box_w, box_h;
    int xalign;             /* Enum.TextXAlignment: 0 left, 1 right, 2 centre */
    int yalign;             /* Enum.TextYAlignment: 0 top, 1 centre, 2 bottom */
    bool wrap;
    bool truncate;          /* TextTruncate.AtEnd */
} ub_text_style;

typedef struct {
    ub_tglyph *g;
    int n, cap;
    ub_trun *runs;
    int nruns, runs_cap;
    f32 bounds_w, bounds_h; /* TextBounds */
    bool fits;              /* TextFits */
    bool broke_word;        /* a word longer than the line was broken */
    int lines;
    i32 graphemes;
} ub_text_layout;

void ub_text_layout_init(ub_text_layout *l);
void ub_text_layout_free(ub_text_layout *l);
bool ub_text_lay_out(const ub_text_style *s, ub_text_layout *out);
/* TextScaled: the largest whole size from lo to hi at which it fits the
 * box (wrapping at word boundaries); lays it out at that size. */
f32 ub_text_lay_out_scaled(ub_text_style *s, f32 lo, f32 hi, ub_text_layout *out);
/* TextService:GetTextSize: the size of plain text, wrapped to `width`. */
void ub_text_measure(const char *text, u32 len, ub_typeface face, f32 size, f32 width, f32 *w, f32 *h);

/* Draws a laid-out text in the box placed by `t`. `grad` (may be NULL)
 * is the object's UIGradient, applied over the box; `max_graphemes` < 0
 * shows all. `stroke`: TextStrokeColor3/Transparency (transparency >= 1
 * = none), and UIStroke on text (thickness > 0). */
typedef struct {
    u32 color;
    f32 transparency;
    f32 thickness;          /* 0: Roblox's one-pixel text stroke */
} ub_text_stroke;
void ub_text_draw(ub_canvas *c, const ub_xform *t, const ub_text_layout *l, f32 box_w, f32 box_h,
                  const ub_paint *grad, i32 max_graphemes, const ub_text_stroke *stroke, f32 alpha_scale);

/* The caches (after a resolution or font change, or to free memory). */
void ub_text_flush(void);

#endif
