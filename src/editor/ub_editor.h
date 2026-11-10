/* Uniblox - the editor's model: a place being built, what is selected,
 * and every change you can make to it, with undo.
 *
 * Nothing here draws or reads input; the editor app (app/app_editor.c)
 * turns clicks and keys into these calls, and the tests make the same
 * calls to build places.
 *
 * Positions inside are Uniblox's (the world's own); the app shows them in
 * Roblox's axes, as scripts see them.
 *
 * Behaviours make parts do things without writing code: each is a short
 * Roblox-style script put inside the part (kill brick, checkpoint, finish
 * line, spinner, mover, disappearing platform, bounce pad, speed pad,
 * door). Its code can still be opened and changed. */
#ifndef UB_EDITOR_H
#define UB_EDITOR_H

#include "world/ub_world.h"

#define ED_UNDO_MAX 64
#define ED_UNDO_BYTES (48u << 20)

typedef enum {
    ED_INS_BLOCK, ED_INS_WEDGE, ED_INS_CORNER_WEDGE, ED_INS_CYLINDER, ED_INS_BALL, ED_INS_SPAWN,
    ED_INS_MODEL, ED_INS_FOLDER, ED_INS_SCRIPT, ED_INS_COUNT
} ed_insert_kind;

typedef enum {
    ED_BEH_KILL, ED_BEH_CHECKPOINT, ED_BEH_FINISH, ED_BEH_SPIN, ED_BEH_MOVE, ED_BEH_DISAPPEAR,
    ED_BEH_BOUNCE, ED_BEH_SPEED, ED_BEH_DOOR, ED_BEH_COUNT
} ed_behaviour;

typedef struct {
    u8 *data;            /* the place as a .ubx file */
    u32 size;
    i32 *sel;            /* selection, as indices in that file */
    i32 sel_count;
    char what[40];
    f32 when;
} ed_snapshot;

typedef struct {
    ub_world world;
    i32 *sel;            /* selected instances */
    i32 sel_count, sel_cap;

    ed_snapshot undo[ED_UNDO_MAX], redo[ED_UNDO_MAX];
    int undo_n, redo_n;
    u32 undo_bytes;

    ub_world clip;       /* copied instances (roots under -1) */
    bool has_clip;

    f32 grid;            /* moves snap to this many studs (0 = free) */
    f32 rot_step;        /* degrees per rotate */
    f32 now;             /* seconds, set by the app (merges quick repeated changes in undo) */
    bool dirty;          /* changed since loaded or saved */
    u32 version;         /* counts changes to the place */
    u32 sel_version;     /* counts changes to the selection */
} ub_editor;

void ed_init(ub_editor *e);          /* a new place: baseplate, spawn, lighting */
void ed_free(ub_editor *e);
void ed_new_place(ub_editor *e);
/* Replaces the place with a .ubx (or Roblox .rbxl/.rbxm when roblox). */
ub_load_result ed_load(ub_editor *e, const u8 *data, u32 size, bool roblox);
bool ed_save(ub_editor *e, u8 **data, u32 *size);  /* plat_alloc'd */
/* A copy of the place to play-test (the editor's own stays untouched). */
bool ed_play_world(const ub_editor *e, ub_world *out);

/* ---------------------------------------------------------- selection */
void ed_select_none(ub_editor *e);
/* add: keep the rest selected (and toggle this one) */
void ed_select(ub_editor *e, i32 inst, bool add);
bool ed_selected(const ub_editor *e, i32 inst);
void ed_select_all(ub_editor *e);    /* everything directly in the Workspace */
/* The part a ray hits first (shown parts in the Workspace); whole_models:
 * report the outermost Model in the Workspace that holds it instead. */
i32 ed_pick(const ub_editor *e, vec3 origin, vec3 dir, bool whole_models, vec3 *point);
/* Box around every selected part (and parts inside selected models). */
bool ed_selection_bounds(const ub_editor *e, vec3 *lo, vec3 *hi);
i32 ed_first_part(const ub_editor *e);   /* first selected part, -1 if none */
i32 ed_first(const ub_editor *e);        /* first selected instance, -1 if none */

/* -------------------------------------------------------------- undo */
/* Call before changing the place. Repeats of the same change within a
 * second (dragging a slider) become one undo step. */
void ed_checkpoint(ub_editor *e, const char *what);
bool ed_undo(ub_editor *e);
bool ed_redo(ub_editor *e);
const char *ed_undo_name(const ub_editor *e);  /* NULL if nothing */
const char *ed_redo_name(const ub_editor *e);

/* ----------------------------------------------------------- changes */
/* These record their own undo step. */
i32 ed_insert(ub_editor *e, ed_insert_kind kind, vec3 pos);
int ed_add_behaviour(ub_editor *e, ed_behaviour b);  /* to every selected part; returns how many */
void ed_delete(ub_editor *e);
void ed_duplicate(ub_editor *e);
void ed_copy(ub_editor *e);
void ed_cut(ub_editor *e);
void ed_paste(ub_editor *e);
void ed_group(ub_editor *e);
void ed_ungroup(ub_editor *e);
void ed_rotate(ub_editor *e, int axis, f32 degrees);   /* about the selection's middle; axis 0..2 */
/* Moves are made in small steps while dragging: call ed_checkpoint once
 * when the drag starts. */
void ed_translate(ub_editor *e, vec3 delta);
/* Grow (or shrink) one face of a part: face 0..5 = +x +y +z -x -y -z in
 * the part's own axes; the opposite face stays put. */
void ed_resize(ub_editor *e, i32 inst, int face, f32 amount);

/* the selection's parts */
void ed_set_color(ub_editor *e, u32 rgb);
void ed_set_material(ub_editor *e, u8 material);
void ed_set_transparency(ub_editor *e, f32 t);
void ed_set_anchored(ub_editor *e, bool on);
void ed_set_collide(ub_editor *e, bool on);
void ed_set_glow(ub_editor *e, bool on);
void ed_set_shape(ub_editor *e, u8 shape);
/* one instance */
void ed_set_name(ub_editor *e, i32 inst, const char *name);
void ed_set_source(ub_editor *e, i32 inst, const char *source);
/* a part's placement in Roblox's terms (Position, Orientation in degrees, Size) */
void ed_set_rbx(ub_editor *e, i32 inst, vec3 pos, vec3 orientation, vec3 size);
void ed_get_rbx(const ub_editor *e, i32 inst, vec3 *pos, vec3 *orientation, vec3 *size);

f32 ed_snap(const ub_editor *e, f32 v);
const char *ed_insert_name(ed_insert_kind k);
const char *ed_behaviour_name(ed_behaviour b);
const char *ed_behaviour_help(ed_behaviour b);
const char *ed_behaviour_source(ed_behaviour b);

#endif
