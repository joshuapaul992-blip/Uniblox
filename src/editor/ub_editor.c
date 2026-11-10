/* Uniblox - the editor's model (see ub_editor.h). */
#include "editor/ub_editor.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "physics/ub_pmath.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "world/ub_query.h"
#include "world/ub_rbxl.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

#define W (&e->world)

static bool str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool grow(void **p, i32 *cap, i32 need, size_t elem)
{
    if (need <= *cap) return true;
    i32 nc = *cap ? *cap * 2 : 32;
    while (nc < need) nc *= 2;
    void *np = plat_alloc((size_t)nc * elem);
    if (!np) return false;
    if (*p) {
        memcpy(np, *p, (size_t)*cap * elem);
        plat_free(*p);
    }
    *p = np;
    *cap = nc;
    return true;
}

/* everything in a subtree, plat_alloc'd */
static i32 *subtree(const ub_world *w, i32 root, i32 *count)
{
    i32 cap = 64, n = 0, done = 0;
    i32 *list = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    if (!list) return NULL;
    list[n++] = root;
    while (done < n) {
        i32 i = list[done++];
        for (i32 c = w->inst[i].first_child; c >= 0; c = w->inst[c].next_sibling) {
            if (n == cap && !grow((void **)&list, &cap, n + 1, sizeof(i32))) {
                plat_free(list);
                return NULL;
            }
            list[n++] = c;
        }
    }
    *count = n;
    return list;
}

static i32 ensure_root(ub_world *w, ub_class cls, const char *class_name)
{
    for (i32 i = 0; i < w->count; ++i)
        if (w->inst[i].parent < 0 && ub_world_alive(w, i) && str_eq(ub_world_str(w, w->inst[i].class_name), class_name))
            return i;
    return ub_world_add(w, -1, cls, class_name, class_name);
}

/* ------------------------------------------------------------- place */

static void clear_history(ub_editor *e)
{
    for (int i = 0; i < e->undo_n; ++i) {
        plat_free(e->undo[i].data);
        plat_free(e->undo[i].sel);
    }
    for (int i = 0; i < e->redo_n; ++i) {
        plat_free(e->redo[i].data);
        plat_free(e->redo[i].sel);
    }
    e->undo_n = e->redo_n = 0;
    e->undo_bytes = 0;
}

static ub_part make_part(u8 shape, vec3 pos, vec3 size, u32 color, u8 material)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = pos;
    p.rot = mat3_identity();
    p.size = size;
    p.color = color;
    p.shape = shape;
    p.material = material;
    p.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    return p;
}

void ed_new_place(ub_editor *e)
{
    ub_world_free(W);
    ub_world_init(W);
    ub_fmt(W->name, sizeof W->name, "New place");
    i32 ws = ub_world_workspace(W);
    ub_world_add(W, -1, UB_CLASS_LIGHTING, NULL, "Lighting");
    ub_world_add(W, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    ub_part base = make_part(UB_SHAPE_BOX, v3(0, -0.5f, 0), v3(256, 1, 256), 0x6E6E6E, UB_MAT_GRID);
    ub_world_add_part(W, ws, UB_CLASS_PART, NULL, "Baseplate", &base);
    ub_part sp = make_part(UB_SHAPE_BOX, v3(0, 0.5f, 0), v3(6, 1, 6), 0xA3A2A5, UB_MAT_PLASTIC);
    ub_world_add_part(W, ws, UB_CLASS_SPAWN, "SpawnLocation", "SpawnLocation", &sp);
    e->sel_count = 0;
    e->sel_version++;
    e->version++;
    e->dirty = false;
    clear_history(e);
}

void ed_init(ub_editor *e)
{
    memset(e, 0, sizeof *e);
    ub_world_init(W);
    ub_world_init(&e->clip);
    e->grid = 1.0f;
    e->rot_step = 90.0f;
    ed_new_place(e);
}

void ed_free(ub_editor *e)
{
    clear_history(e);
    ub_world_free(W);
    ub_world_free(&e->clip);
    plat_free(e->sel);
    memset(e, 0, sizeof *e);
}

ub_load_result ed_load(ub_editor *e, const u8 *data, u32 size, bool roblox)
{
    ub_world w;
    ub_world_init(&w);
    ub_import_stats st;
    ub_load_result r = roblox ? ub_rbxl_load(&w, data, size, &st) : ub_ubx_load(&w, data, size);
    if (r != UB_LOAD_OK) {
        ub_world_free(&w);
        return r;
    }
    ub_world_free(W);
    *W = w;
    ub_world_workspace(W);
    e->sel_count = 0;
    e->sel_version++;
    e->version++;
    e->dirty = false;
    clear_history(e);
    return UB_LOAD_OK;
}

bool ed_save(ub_editor *e, u8 **data, u32 *size)
{
    if (!ub_ubx_save(W, data, size)) return false;
    e->dirty = false;
    return true;
}

bool ed_play_world(const ub_editor *e, ub_world *out)
{
    u8 *data;
    u32 size;
    ub_world_init(out);
    if (!ub_ubx_save(W, &data, &size)) return false;
    bool ok = ub_ubx_load(out, data, size) == UB_LOAD_OK;
    plat_free(data);
    return ok;
}

/* ---------------------------------------------------------- selection */

void ed_select_none(ub_editor *e)
{
    if (e->sel_count) e->sel_version++;
    e->sel_count = 0;
}

bool ed_selected(const ub_editor *e, i32 inst)
{
    for (i32 i = 0; i < e->sel_count; ++i)
        if (e->sel[i] == inst) return true;
    return false;
}

void ed_select(ub_editor *e, i32 inst, bool add)
{
    if (!ub_world_alive(W, inst)) return;
    e->sel_version++;
    if (!add) e->sel_count = 0;
    for (i32 i = 0; i < e->sel_count; ++i)
        if (e->sel[i] == inst) { /* toggle off */
            e->sel[i] = e->sel[--e->sel_count];
            return;
        }
    if (!grow((void **)&e->sel, &e->sel_cap, e->sel_count + 1, sizeof(i32))) return;
    e->sel[e->sel_count++] = inst;
}

void ed_select_all(ub_editor *e)
{
    e->sel_count = 0;
    i32 ws = ub_world_workspace(W);
    for (i32 c = W->inst[ws].first_child; c >= 0; c = W->inst[c].next_sibling) ed_select(e, c, true);
    e->sel_version++;
}

i32 ed_first(const ub_editor *e) { return e->sel_count ? e->sel[0] : -1; }

i32 ed_first_part(const ub_editor *e)
{
    for (i32 i = 0; i < e->sel_count; ++i)
        if (ub_world_alive(W, e->sel[i]) && W->inst[e->sel[i]].part >= 0) return e->sel[i];
    return -1;
}

i32 ed_pick(const ub_editor *e, vec3 origin, vec3 dir, bool whole_models, vec3 *point)
{
    i32 ws = W->workspace;
    i32 best = -1;
    f32 best_t = 1e30f;
    for (i32 i = 0; i < W->count; ++i) {
        const ub_instance *in = &W->inst[i];
        if (in->part < 0 || !ub_world_alive(W, i)) continue;
        const ub_part *p = &W->parts[in->part];
        if (p->transparency >= 250) continue;
        f32 t;
        if (!ub_part_raycast(p, origin, dir, best_t, &t) || t >= best_t) continue;
        if (!ub_world_is_descendant(W, i, ws)) continue;
        best_t = t;
        best = i;
    }
    if (best < 0) return -1;
    if (point) *point = v3_madd(origin, dir, best_t);
    if (whole_models) { /* the outermost model inside the Workspace */
        i32 model = -1;
        for (i32 a = W->inst[best].parent; a >= 0 && a != ws; a = W->inst[a].parent)
            if (W->inst[a].cls == UB_CLASS_MODEL) model = a;
        if (model >= 0) return model;
    }
    return best;
}

/* selected instances not inside another selected one */
static i32 sel_roots(const ub_editor *e, i32 *out)
{
    i32 n = 0;
    for (i32 i = 0; i < e->sel_count; ++i) {
        i32 s = e->sel[i];
        if (!ub_world_alive(W, s)) continue;
        bool inside = false;
        for (i32 a = W->inst[s].parent; a >= 0 && !inside; a = W->inst[a].parent) inside = ed_selected(e, a);
        if (!inside) out[n++] = s;
    }
    return n;
}

/* every part in the selection (including inside selected models) */
static i32 *sel_parts(const ub_editor *e, i32 *count)
{
    i32 cap = 64, n = 0;
    i32 *list = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!list || !roots) {
        plat_free(list);
        plat_free(roots);
        return NULL;
    }
    i32 nr = sel_roots(e, roots);
    for (i32 r = 0; r < nr; ++r) {
        i32 k;
        i32 *sub = subtree(W, roots[r], &k);
        if (!sub) continue;
        for (i32 j = 0; j < k; ++j)
            if (W->inst[sub[j]].part >= 0 && grow((void **)&list, &cap, n + 1, sizeof(i32))) list[n++] = sub[j];
        plat_free(sub);
    }
    plat_free(roots);
    *count = n;
    return list;
}

static void part_aabb(const ub_part *p, vec3 *lo, vec3 *hi)
{
    vec3 h = v3_scale(p->size, 0.5f);
    vec3 ext = v3(ub_absf(p->rot.x.x) * h.x + ub_absf(p->rot.y.x) * h.y + ub_absf(p->rot.z.x) * h.z,
                  ub_absf(p->rot.x.y) * h.x + ub_absf(p->rot.y.y) * h.y + ub_absf(p->rot.z.y) * h.z,
                  ub_absf(p->rot.x.z) * h.x + ub_absf(p->rot.y.z) * h.y + ub_absf(p->rot.z.z) * h.z);
    *lo = v3_sub(p->pos, ext);
    *hi = v3_add(p->pos, ext);
}

static bool parts_bounds(const ub_world *w, const i32 *parts, i32 n, vec3 *lo, vec3 *hi)
{
    *lo = v3(1e30f, 1e30f, 1e30f);
    *hi = v3(-1e30f, -1e30f, -1e30f);
    for (i32 i = 0; i < n; ++i) {
        vec3 a, b;
        part_aabb(&w->parts[w->inst[parts[i]].part], &a, &b);
        *lo = v3_min(*lo, a);
        *hi = v3_max(*hi, b);
    }
    return n > 0;
}

bool ed_selection_bounds(const ub_editor *e, vec3 *lo, vec3 *hi)
{
    i32 n;
    i32 *parts = sel_parts(e, &n);
    if (!parts) return false;
    bool ok = parts_bounds(W, parts, n, lo, hi);
    plat_free(parts);
    return ok;
}

/* -------------------------------------------------------------- undo */

static void free_snap(ed_snapshot *s)
{
    plat_free(s->data);
    plat_free(s->sel);
    memset(s, 0, sizeof *s);
}

static bool take_snapshot(ub_editor *e, ed_snapshot *s, const char *what)
{
    memset(s, 0, sizeof *s);
    if (!ub_ubx_save(W, &s->data, &s->size)) return false;
    i32 *remap = (i32 *)plat_alloc(sizeof(i32) * (size_t)(W->count + 1));
    s->sel = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (remap && s->sel && ub_world_save_remap(W, remap)) {
        for (i32 i = 0; i < e->sel_count; ++i)
            if (e->sel[i] >= 0 && e->sel[i] < W->count && remap[e->sel[i]] >= 0) s->sel[s->sel_count++] = remap[e->sel[i]];
    }
    plat_free(remap);
    ub_fmt(s->what, sizeof s->what, "%s", what);
    s->when = e->now;
    return true;
}

static bool restore_snapshot(ub_editor *e, const ed_snapshot *s)
{
    ub_world w;
    ub_world_init(&w);
    if (ub_ubx_load(&w, s->data, s->size) != UB_LOAD_OK) {
        ub_world_free(&w);
        return false;
    }
    ub_world_free(W);
    *W = w;
    ub_world_workspace(W);
    e->sel_count = 0;
    for (i32 i = 0; i < s->sel_count; ++i) ed_select(e, s->sel[i], true);
    e->sel_version++;
    e->version++;
    e->dirty = true;
    return true;
}

static void drop_oldest(ed_snapshot *list, int *n, u32 *bytes)
{
    *bytes -= list[0].size;
    free_snap(&list[0]);
    memmove(list, list + 1, sizeof(ed_snapshot) * (size_t)(*n - 1));
    --*n;
}

void ed_checkpoint(ub_editor *e, const char *what)
{
    e->dirty = true;
    e->version++;
    if (e->undo_n && what[0] && str_eq(e->undo[e->undo_n - 1].what, what) && e->now - e->undo[e->undo_n - 1].when < 1.0f) {
        e->undo[e->undo_n - 1].when = e->now; /* the same change, continued */
        return;
    }
    for (int i = 0; i < e->redo_n; ++i) free_snap(&e->redo[i]);
    e->redo_n = 0;
    if (e->undo_n == ED_UNDO_MAX) drop_oldest(e->undo, &e->undo_n, &e->undo_bytes);
    if (!take_snapshot(e, &e->undo[e->undo_n], what)) return;
    e->undo_bytes += e->undo[e->undo_n].size;
    e->undo_n++;
    while (e->undo_bytes > ED_UNDO_BYTES && e->undo_n > 1) drop_oldest(e->undo, &e->undo_n, &e->undo_bytes);
}

bool ed_undo(ub_editor *e)
{
    if (!e->undo_n) return false;
    ed_snapshot now;
    if (!take_snapshot(e, &now, e->undo[e->undo_n - 1].what)) return false;
    ed_snapshot *s = &e->undo[e->undo_n - 1];
    if (!restore_snapshot(e, s)) {
        free_snap(&now);
        return false;
    }
    e->undo_bytes -= s->size;
    free_snap(s);
    e->undo_n--;
    if (e->redo_n == ED_UNDO_MAX) {
        u32 dummy = 0;
        drop_oldest(e->redo, &e->redo_n, &dummy);
    }
    e->redo[e->redo_n++] = now;
    return true;
}

bool ed_redo(ub_editor *e)
{
    if (!e->redo_n) return false;
    ed_snapshot now;
    if (!take_snapshot(e, &now, e->redo[e->redo_n - 1].what)) return false;
    ed_snapshot *s = &e->redo[e->redo_n - 1];
    if (!restore_snapshot(e, s)) {
        free_snap(&now);
        return false;
    }
    free_snap(s);
    e->redo_n--;
    now.when = -100; /* never merged with a new change */
    if (e->undo_n == ED_UNDO_MAX) drop_oldest(e->undo, &e->undo_n, &e->undo_bytes);
    e->undo[e->undo_n++] = now;
    e->undo_bytes += now.size;
    return true;
}

const char *ed_undo_name(const ub_editor *e) { return e->undo_n ? e->undo[e->undo_n - 1].what : NULL; }
const char *ed_redo_name(const ub_editor *e) { return e->redo_n ? e->redo[e->redo_n - 1].what : NULL; }

/* ----------------------------------------------------------- changes */

f32 ed_snap(const ub_editor *e, f32 v)
{
    if (e->grid <= 0) return v;
    return (f32)ub_floorf(v / e->grid + 0.5f) * e->grid;
}

static const struct {
    const char *name, *class_name;
    u8 shape;
    f32 sx, sy, sz;
} k_ins[] = {
    { "Block", "Part", UB_SHAPE_BOX, 4, 1, 2 },
    { "Wedge", "WedgePart", UB_SHAPE_WEDGE, 4, 1, 2 },
    { "Corner wedge", "CornerWedgePart", UB_SHAPE_CORNER_WEDGE, 2, 2, 2 },
    { "Cylinder", "Part", UB_SHAPE_CYLINDER, 2, 4, 2 },
    { "Ball", "Part", UB_SHAPE_BALL, 2, 2, 2 },
    { "Spawn point", "SpawnLocation", UB_SHAPE_BOX, 6, 1, 6 },
    { "Model", "Model", 0, 0, 0, 0 },
    { "Folder", "Folder", 0, 0, 0, 0 },
    { "Script", "Script", 0, 0, 0, 0 },
};

const char *ed_insert_name(ed_insert_kind k) { return k < ED_INS_COUNT ? k_ins[k].name : "?"; }

/* where new things go: inside a selected Model or Folder in the Workspace, else the Workspace */
static i32 container(const ub_editor *e)
{
    i32 s = ed_first(e);
    i32 ws = W->workspace;
    if (s >= 0 && ub_world_alive(W, s) && (W->inst[s].cls == UB_CLASS_MODEL || W->inst[s].cls == UB_CLASS_FOLDER) &&
        (s == ws || ub_world_is_descendant(W, s, ws)))
        return s;
    return ws;
}

i32 ed_insert(ub_editor *e, ed_insert_kind kind, vec3 pos)
{
    if (kind >= ED_INS_COUNT) return -1;
    ed_checkpoint(e, "Insert");
    i32 idx;
    if (kind == ED_INS_SCRIPT) {
        i32 s = ed_first(e);
        i32 parent = s >= 0 && ub_world_alive(W, s) && s != W->workspace && W->inst[s].source == UB_NO_STRING
                         ? s : ensure_root(W, UB_CLASS_FOLDER, "ServerScriptService");
        idx = ub_world_add(W, parent, UB_CLASS_OTHER, "Script", "Script");
        if (idx >= 0) ub_world_set_source(W, idx, "print(\"Hello from \" .. script:GetFullName())\n");
    } else if (kind == ED_INS_MODEL || kind == ED_INS_FOLDER) {
        idx = ub_world_add(W, container(e), kind == ED_INS_MODEL ? UB_CLASS_MODEL : UB_CLASS_FOLDER,
                           k_ins[kind].class_name, k_ins[kind].class_name);
    } else {
        vec3 size = v3(k_ins[kind].sx, k_ins[kind].sy, k_ins[kind].sz);
        vec3 at = v3(ed_snap(e, pos.x), ed_snap(e, pos.y) + size.y * 0.5f, ed_snap(e, pos.z));
        ub_part p = make_part(k_ins[kind].shape, at, size, 0xA3A2A5, UB_MAT_PLASTIC);
        bool spawn = kind == ED_INS_SPAWN;
        idx = ub_world_add_part(W, container(e), spawn ? UB_CLASS_SPAWN : UB_CLASS_PART, k_ins[kind].class_name,
                                spawn ? "SpawnLocation" : "Part", &p);
    }
    if (idx >= 0) ed_select(e, idx, false);
    return idx;
}

/* ---- behaviours ---- */

static const struct {
    const char *name, *script, *help;
    u32 color;     /* 0: leave the colour */
    bool glow;
    const char *source;
} k_beh[ED_BEH_COUNT] = {
    { "Kill brick", "KillBrick", "Touching it knocks the player out", 0xFF3A1C, true,
      "-- Kill brick: touching it knocks the player out\n"
      "local part = script.Parent\n"
      "part.Touched:Connect(function(hit)\n"
      "\tlocal humanoid = hit.Parent:FindFirstChildOfClass(\"Humanoid\")\n"
      "\tif humanoid and humanoid.Health > 0 then\n"
      "\t\thumanoid.Health = 0\n"
      "\tend\n"
      "end)\n" },
    { "Checkpoint", "Checkpoint", "Touch it: you come back here after falling", 0x4B974B, false,
      "-- Checkpoint: touch it and you come back here after falling\n"
      "local part = script.Parent\n"
      "local Players = game:GetService(\"Players\")\n"
      "part.Touched:Connect(function(hit)\n"
      "\tlocal player = Players:GetPlayerFromCharacter(hit.Parent)\n"
      "\tif player and player.RespawnLocation ~= part then\n"
      "\t\tplayer.RespawnLocation = part\n"
      "\t\tpart.BrickColor = BrickColor.new(\"Lime green\")\n"
      "\t\tprint(\"Checkpoint reached!\")\n"
      "\tend\n"
      "end)\n" },
    { "Finish line", "FinishLine", "Says how long the course took", 0xF5CD30, true,
      "-- Finish line: says how long the course took\n"
      "local part = script.Parent\n"
      "local done = {}\n"
      "part.Touched:Connect(function(hit)\n"
      "\tlocal humanoid = hit.Parent:FindFirstChildOfClass(\"Humanoid\")\n"
      "\tif humanoid and humanoid.Health > 0 and not done[hit.Parent] then\n"
      "\t\tdone[hit.Parent] = true\n"
      "\t\tprint(string.format(\"%s finished the course in %.1f seconds!\", hit.Parent.Name, time()))\n"
      "\tend\n"
      "end)\n" },
    { "Spinner", "Spinner", "Turns round and round", 0, false,
      "-- Spinner: turns round and round\n"
      "local SPEED = 90 -- degrees per second\n"
      "local part = script.Parent\n"
      "local start = part.CFrame\n"
      "local angle = 0\n"
      "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n"
      "\tangle += math.rad(SPEED) * dt\n"
      "\tpart.CFrame = start * CFrame.Angles(0, angle, 0)\n"
      "end)\n" },
    { "Mover", "Mover", "Slides back and forth (carries players)", 0, false,
      "-- Mover: slides back and forth, carrying whoever stands on it\n"
      "local DISTANCE = 12 -- studs, towards the part's right\n"
      "local TIME = 2 -- seconds each way\n"
      "local part = script.Parent\n"
      "local goal = part.CFrame * CFrame.new(DISTANCE, 0, 0)\n"
      "local info = TweenInfo.new(TIME, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)\n"
      "game:GetService(\"TweenService\"):Create(part, info, { CFrame = goal }):Play()\n" },
    { "Disappearing", "Disappearing", "Fades away when stepped on, then comes back", 0, false,
      "-- Disappearing platform: fades away when stepped on, then comes back\n"
      "local part = script.Parent\n"
      "local busy = false\n"
      "part.Touched:Connect(function(hit)\n"
      "\tif busy or not hit.Parent:FindFirstChildOfClass(\"Humanoid\") then return end\n"
      "\tbusy = true\n"
      "\tfor t = 0, 1, 0.1 do part.Transparency = t task.wait(0.05) end\n"
      "\tpart.CanCollide = false\n"
      "\ttask.wait(3)\n"
      "\tpart.CanCollide = true\n"
      "\tpart.Transparency = 0\n"
      "\tbusy = false\n"
      "end)\n" },
    { "Bounce pad", "BouncePad", "Launches the player upwards", 0x3CC83C, false,
      "-- Bounce pad: launches the player upwards\n"
      "local POWER = 110 -- studs per second\n"
      "local part = script.Parent\n"
      "part.Touched:Connect(function(hit)\n"
      "\tlocal root = hit.Parent:FindFirstChild(\"HumanoidRootPart\")\n"
      "\tif root and hit.Parent:FindFirstChildOfClass(\"Humanoid\") then\n"
      "\t\tlocal v = root.AssemblyLinearVelocity\n"
      "\t\troot.AssemblyLinearVelocity = Vector3.new(v.X, POWER, v.Z)\n"
      "\tend\n"
      "end)\n" },
    { "Speed pad", "SpeedPad", "Run faster for a few seconds", 0xF5A623, false,
      "-- Speed pad: run faster for a few seconds\n"
      "local part = script.Parent\n"
      "part.Touched:Connect(function(hit)\n"
      "\tlocal humanoid = hit.Parent:FindFirstChildOfClass(\"Humanoid\")\n"
      "\tif humanoid and humanoid.WalkSpeed < 40 then\n"
      "\t\thumanoid.WalkSpeed = 40\n"
      "\t\ttask.wait(3)\n"
      "\t\thumanoid.WalkSpeed = 16\n"
      "\tend\n"
      "end)\n" },
    { "Door", "Door", "Opens when touched, closes again", 0, false,
      "-- Door: opens when touched, shuts again after a few seconds\n"
      "local door = script.Parent\n"
      "local open = false\n"
      "door.Touched:Connect(function(hit)\n"
      "\tif open or not hit.Parent:FindFirstChildOfClass(\"Humanoid\") then return end\n"
      "\topen = true\n"
      "\tdoor.CanCollide = false\n"
      "\tdoor.Transparency = 0.7\n"
      "\ttask.wait(3)\n"
      "\tdoor.Transparency = 0\n"
      "\tdoor.CanCollide = true\n"
      "\topen = false\n"
      "end)\n" },
};

const char *ed_behaviour_name(ed_behaviour b) { return b < ED_BEH_COUNT ? k_beh[b].name : "?"; }
const char *ed_behaviour_help(ed_behaviour b) { return b < ED_BEH_COUNT ? k_beh[b].help : ""; }
const char *ed_behaviour_source(ed_behaviour b) { return b < ED_BEH_COUNT ? k_beh[b].source : ""; }

int ed_add_behaviour(ub_editor *e, ed_behaviour b)
{
    if (b >= ED_BEH_COUNT) return 0;
    i32 n;
    i32 *parts = sel_parts(e, &n);
    if (!parts) return 0;
    if (n) ed_checkpoint(e, "Add behaviour");
    int added = 0;
    for (i32 i = 0; i < n; ++i) {
        i32 pi = parts[i];
        if (ub_world_find_child(W, pi, k_beh[b].script) >= 0) continue; /* has it already */
        i32 s = ub_world_add(W, pi, UB_CLASS_OTHER, "Script", k_beh[b].script);
        if (s < 0) continue;
        ub_world_set_source(W, s, k_beh[b].source);
        ub_part *p = &W->parts[W->inst[pi].part];
        if (k_beh[b].color) p->color = k_beh[b].color;
        if (k_beh[b].glow) p->flags |= UB_PART_GLOW;
        added++;
    }
    plat_free(parts);
    return added;
}

/* ---- delete, copy, paste ---- */

static bool deletable(const ub_editor *e, i32 i)
{
    return ub_world_alive(W, i) && W->inst[i].parent >= 0 && i != W->workspace;
}

static void destroy_now(ub_editor *e, i32 root)
{
    i32 n;
    i32 *sub = subtree(W, root, &n);
    ub_world_destroy(W, root);
    if (!sub) return;
    for (i32 j = 0; j < n; ++j) ub_world_reclaim(W, sub[j]); /* the editor owns every index */
    plat_free(sub);
}

void ed_delete(ub_editor *e)
{
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!roots) return;
    i32 n = sel_roots(e, roots), k = 0;
    for (i32 i = 0; i < n; ++i) k += deletable(e, roots[i]);
    if (k) ed_checkpoint(e, "Delete");
    for (i32 i = 0; i < n; ++i)
        if (deletable(e, roots[i])) destroy_now(e, roots[i]);
    plat_free(roots);
    e->sel_count = 0;
    e->sel_version++;
}

/* copies a subtree from one world into another; returns the new root */
static i32 copy_tree(ub_world *dst, i32 dparent, const ub_world *src, i32 root)
{
    i32 *map = (i32 *)plat_alloc(sizeof(i32) * (size_t)(src->count + 1));
    i32 n;
    i32 *sub = subtree(src, root, &n);
    if (!map || !sub) {
        plat_free(map);
        plat_free(sub);
        return -1;
    }
    for (i32 i = 0; i < src->count; ++i) map[i] = -1;
    for (i32 j = 0; j < n; ++j) { /* (breadth first: parents come before children) */
        i32 i = sub[j];
        const ub_instance *in = &src->inst[i];
        i32 np = j == 0 ? dparent : map[in->parent];
        const char *name = ub_world_str(src, in->name), *cn = ub_world_str(src, in->class_name);
        i32 ni;
        if (in->part >= 0) {
            ub_part p = src->parts[in->part];
            p.flags &= (u8)~UB_PART_REMOVED;
            ni = ub_world_add_part(dst, np, (ub_class)in->cls, cn, name, &p);
        } else {
            ni = ub_world_add(dst, np, (ub_class)in->cls, cn, name);
        }
        if (ni < 0) break;
        dst->inst[ni].flags = (u16)(in->flags & UB_INST_SAVED_FLAGS);
        if (in->source != UB_NO_STRING) ub_world_set_source(dst, ni, ub_world_source(src, i));
        map[i] = ni;
    }
    for (i32 k = 0; k < src->weld_count; ++k) {
        i32 a = src->welds[k].a, b = src->welds[k].b;
        if (a < src->count && b < src->count && map[a] >= 0 && map[b] >= 0) ub_world_add_weld(dst, map[a], map[b]);
    }
    i32 r = map[root];
    plat_free(map);
    plat_free(sub);
    return r;
}

void ed_copy(ub_editor *e)
{
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!roots) return;
    i32 n = sel_roots(e, roots);
    if (n) {
        ub_world_free(&e->clip);
        ub_world_init(&e->clip);
        e->has_clip = false;
        for (i32 i = 0; i < n; ++i)
            if (deletable(e, roots[i]) && copy_tree(&e->clip, -1, W, roots[i]) >= 0) e->has_clip = true;
    }
    plat_free(roots);
}

void ed_cut(ub_editor *e)
{
    ed_copy(e);
    ed_delete(e);
    if (e->undo_n) ub_fmt(e->undo[e->undo_n - 1].what, sizeof e->undo[0].what, "Cut");
}

/* moves new copies (the given roots) along +x by their width, so they sit beside the originals */
static void offset_copies(ub_editor *e, const i32 *roots, i32 n)
{
    i32 cap = 64, np = 0;
    i32 *parts = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    if (!parts) return;
    for (i32 r = 0; r < n; ++r) {
        i32 k;
        i32 *sub = subtree(W, roots[r], &k);
        if (!sub) continue;
        for (i32 j = 0; j < k; ++j)
            if (W->inst[sub[j]].part >= 0 && grow((void **)&parts, &cap, np + 1, sizeof(i32))) parts[np++] = sub[j];
        plat_free(sub);
    }
    vec3 lo, hi;
    if (parts_bounds(W, parts, np, &lo, &hi)) {
        f32 dx = hi.x - lo.x;
        if (e->grid > 0) dx = (f32)ub_ceilf(dx / e->grid - 0.001f) * e->grid;
        for (i32 i = 0; i < np; ++i) W->parts[W->inst[parts[i]].part].pos.x += dx;
    }
    plat_free(parts);
}

void ed_duplicate(ub_editor *e)
{
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    i32 *made = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!roots || !made) {
        plat_free(roots);
        plat_free(made);
        return;
    }
    i32 n = sel_roots(e, roots), m = 0;
    if (n) ed_checkpoint(e, "Duplicate");
    for (i32 i = 0; i < n; ++i) {
        if (!deletable(e, roots[i])) continue;
        i32 c = ub_world_clone(W, roots[i]);
        if (c < 0) continue;
        ub_world_set_parent(W, c, W->inst[roots[i]].parent);
        made[m++] = c;
    }
    offset_copies(e, made, m);
    e->sel_count = 0;
    for (i32 i = 0; i < m; ++i) ed_select(e, made[i], true);
    plat_free(roots);
    plat_free(made);
}

void ed_paste(ub_editor *e)
{
    if (!e->has_clip) return;
    ed_checkpoint(e, "Paste");
    i32 parent = container(e);
    i32 cap = 16, m = 0;
    i32 *made = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    for (i32 i = 0; i < e->clip.count && made; ++i) {
        if (e->clip.inst[i].parent >= 0 || !ub_world_alive(&e->clip, i)) continue;
        i32 c = copy_tree(W, parent, &e->clip, i);
        if (c >= 0 && grow((void **)&made, &cap, m + 1, sizeof(i32))) made[m++] = c;
    }
    if (made) {
        offset_copies(e, made, m);
        e->sel_count = 0;
        for (i32 i = 0; i < m; ++i) ed_select(e, made[i], true);
    }
    plat_free(made);
}

/* ---- group ---- */

void ed_group(ub_editor *e)
{
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!roots) return;
    i32 n = sel_roots(e, roots), k = 0;
    for (i32 i = 0; i < n; ++i) k += deletable(e, roots[i]);
    if (k) {
        ed_checkpoint(e, "Group");
        i32 parent = -1;
        for (i32 i = 0; i < n && parent < 0; ++i)
            if (deletable(e, roots[i])) parent = W->inst[roots[i]].parent;
        i32 model = ub_world_add(W, parent, UB_CLASS_MODEL, "Model", "Model");
        for (i32 i = 0; i < n && model >= 0; ++i)
            if (deletable(e, roots[i])) ub_world_set_parent(W, roots[i], model);
        if (model >= 0) ed_select(e, model, false);
    }
    plat_free(roots);
}

void ed_ungroup(ub_editor *e)
{
    i32 *roots = (i32 *)plat_alloc(sizeof(i32) * (size_t)(e->sel_count + 1));
    if (!roots) return;
    i32 n = sel_roots(e, roots);
    bool any = false;
    for (i32 i = 0; i < n; ++i) {
        i32 g = roots[i];
        if (!deletable(e, g) || (W->inst[g].cls != UB_CLASS_MODEL && W->inst[g].cls != UB_CLASS_FOLDER)) continue;
        if (!any) {
            ed_checkpoint(e, "Ungroup");
            e->sel_count = 0;
            any = true;
        }
        i32 parent = W->inst[g].parent;
        while (W->inst[g].first_child >= 0) {
            i32 c = W->inst[g].first_child;
            ub_world_set_parent(W, c, parent);
            ed_select(e, c, true);
        }
        destroy_now(e, g);
    }
    plat_free(roots);
    e->sel_version++;
}

/* ---- moving ---- */

void ed_translate(ub_editor *e, vec3 delta)
{
    i32 n;
    i32 *parts = sel_parts(e, &n);
    if (!parts) return;
    for (i32 i = 0; i < n; ++i) {
        ub_part *p = &W->parts[W->inst[parts[i]].part];
        p->pos = v3_add(p->pos, delta);
    }
    if (n) {
        e->version++;
        e->dirty = true;
    }
    plat_free(parts);
}

void ed_rotate(ub_editor *e, int axis, f32 degrees)
{
    i32 n;
    i32 *parts = sel_parts(e, &n);
    if (!parts) return;
    vec3 lo, hi;
    if (n && parts_bounds(W, parts, n, &lo, &hi)) {
        ed_checkpoint(e, "Rotate");
        vec3 c = v3_scale(v3_add(lo, hi), 0.5f);
        f32 a = degrees * (UB_PI / 180.0f), ca = ub_cosf(a), sa = ub_sinf(a);
        mat3 r = mat3_identity();
        if (axis == 0) { r.y = v3(0, ca, sa); r.z = v3(0, -sa, ca); }
        else if (axis == 1) { r.x = v3(ca, 0, -sa); r.z = v3(sa, 0, ca); }
        else { r.x = v3(ca, sa, 0); r.y = v3(-sa, ca, 0); }
        for (i32 i = 0; i < n; ++i) {
            ub_part *p = &W->parts[W->inst[parts[i]].part];
            p->pos = v3_add(c, mat3_apply(&r, v3_sub(p->pos, c)));
            mat3 nr = mat3_mul(&r, &p->rot);
            p->rot = quat_to_mat3(quat_normalize(quat_from_mat3(&nr))); /* no drift */
            /* clean near-axis values, so 90 degree turns stay exact */
            f32 *m = &p->rot.x.x;
            for (int k = 0; k < 9; ++k) {
                if (ub_absf(m[k]) < 1e-5f) m[k] = 0;
                if (ub_absf(m[k] - 1) < 1e-5f) m[k] = 1;
                if (ub_absf(m[k] + 1) < 1e-5f) m[k] = -1;
            }
        }
    }
    plat_free(parts);
}

void ed_resize(ub_editor *e, i32 inst, int face, f32 amount)
{
    if (!ub_world_alive(W, inst) || W->inst[inst].part < 0 || face < 0 || face > 5) return;
    ub_part *p = &W->parts[W->inst[inst].part];
    int axis = face % 3;
    f32 sign = face < 3 ? 1.0f : -1.0f;
    f32 *sz = &p->size.x;
    f32 old = sz[axis];
    f32 nv = old + amount;
    if (nv < 0.2f) nv = 0.2f;
    if (nv > 2048) nv = 2048;
    f32 grow_by = nv - old;
    if (grow_by == 0) return;
    if (p->shape == UB_SHAPE_BALL) { /* stays round, grows from the middle */
        p->size = v3(nv, nv, nv);
    } else {
        sz[axis] = nv;
        if (p->shape == UB_SHAPE_CYLINDER && axis != 1) { /* round across */
            p->size.x = p->size.z = nv;
        }
        vec3 ax = axis == 0 ? p->rot.x : axis == 1 ? p->rot.y : p->rot.z;
        p->pos = v3_madd(p->pos, ax, sign * grow_by * 0.5f);
    }
    e->version++;
    e->dirty = true;
}

/* ---- properties ---- */

typedef void (*part_fn)(ub_part *p, const void *arg);

static void each_part(ub_editor *e, const char *what, part_fn fn, const void *arg)
{
    i32 n;
    i32 *parts = sel_parts(e, &n);
    if (!parts) return;
    if (n) ed_checkpoint(e, what);
    for (i32 i = 0; i < n; ++i) fn(&W->parts[W->inst[parts[i]].part], arg);
    plat_free(parts);
}

static void f_color(ub_part *p, const void *a) { p->color = *(const u32 *)a & 0xFFFFFF; }
static void f_material(ub_part *p, const void *a) { p->material = *(const u8 *)a; }
static void f_transp(ub_part *p, const void *a)
{
    f32 t = ub_clampf(*(const f32 *)a, 0, 1);
    p->transparency = (u8)(t * 255.0f + 0.5f);
}
static void f_flag(ub_part *p, const void *a)
{
    const u8 *f = (const u8 *)a; /* [flag, on] */
    if (f[1]) p->flags |= f[0];
    else p->flags &= (u8)~f[0];
}
static void f_shape(ub_part *p, const void *a)
{
    u8 s = *(const u8 *)a;
    if (s == p->shape) return;
    vec3 pos, sz = ub_part_get_rbx_size(p);
    mat3 r;
    ub_part_get_rbx(p, &pos, &r);
    p->shape = s;
    ub_part_set_rbx_size(p, sz);
    ub_part_set_rbx(p, pos, &r);
}

void ed_set_color(ub_editor *e, u32 rgb) { each_part(e, "Colour", f_color, &rgb); }
void ed_set_material(ub_editor *e, u8 m) { each_part(e, "Material", f_material, &m); }
void ed_set_transparency(ub_editor *e, f32 t) { each_part(e, "Transparency", f_transp, &t); }
void ed_set_shape(ub_editor *e, u8 s) { each_part(e, "Shape", f_shape, &s); }

void ed_set_anchored(ub_editor *e, bool on)
{
    u8 a[2] = { UB_PART_ANCHORED, on };
    each_part(e, "Anchored", f_flag, a);
}

void ed_set_collide(ub_editor *e, bool on)
{
    u8 a[2] = { UB_PART_CAN_COLLIDE, on };
    each_part(e, "CanCollide", f_flag, a);
}

void ed_set_glow(ub_editor *e, bool on)
{
    u8 a[2] = { UB_PART_GLOW, on };
    each_part(e, "Neon", f_flag, a);
}

void ed_set_name(ub_editor *e, i32 inst, const char *name)
{
    if (!ub_world_alive(W, inst) || !name[0]) return;
    ed_checkpoint(e, "Rename");
    ub_world_set_name(W, inst, name);
}

void ed_set_source(ub_editor *e, i32 inst, const char *source)
{
    if (!ub_world_alive(W, inst)) return;
    ed_checkpoint(e, "Edit script");
    ub_world_set_source(W, inst, source);
}

void ed_get_rbx(const ub_editor *e, i32 inst, vec3 *pos, vec3 *orientation, vec3 *size)
{
    const ub_part *p = &W->parts[W->inst[inst].part];
    mat3 r;
    ub_part_get_rbx(p, pos, &r);
    *orientation = ub_rbx_orientation(&r);
    *size = ub_part_get_rbx_size(p);
}

void ed_set_rbx(ub_editor *e, i32 inst, vec3 pos, vec3 orientation, vec3 size)
{
    if (!ub_world_alive(W, inst) || W->inst[inst].part < 0) return;
    ed_checkpoint(e, "Move");
    ub_part *p = &W->parts[W->inst[inst].part];
    mat3 r = ub_rbx_from_orientation(orientation);
    ub_part_set_rbx_size(p, size);
    ub_part_set_rbx(p, pos, &r);
}
