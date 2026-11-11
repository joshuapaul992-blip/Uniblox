#include "game/ub_game.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "gui/ub_gui.h"
#include "world/ub_classes.h"
#include "platform/platform.h"
#include "script/ub_script.h"
#include "crt/ub_crt.h"
#include "net/ub_netbuf.h"

#define ROOT_HEIGHT 3.0f   /* HumanoidRootPart centre above the feet */

/* Are the game's scripts running? (Then their runtime is the one in use.) */
static bool scr(const ub_game *g)
{
    if (!g->scripts) return false;
    ub_script_use(g->srt);
    return true;
}

static bool grow(void **p, i32 *cap, i32 need, size_t elem)
{
    if (need <= *cap) return true;
    i32 nc = *cap ? *cap * 2 : 256;
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

static bool in_workspace(const ub_game *g, i32 inst)
{
    return ub_world_alive(&g->world, inst) && ub_world_is_descendant(&g->world, inst, g->world.workspace);
}

static void make_live(ub_game *g, i32 part)
{
    if (part < 0) return;
    if (!grow((void **)&g->live_flag, &g->live_flag_cap, part + 1, 1)) return;
    if (g->live_flag[part]) return;
    if (!grow((void **)&g->live, &g->live_cap, g->live_count + 1, sizeof(i32))) return;
    g->live_flag[part] = 1;
    g->live[g->live_count++] = part;
    g->statics_dirty = true;
}

bool ub_game_part_live(const ub_game *g, i32 part)
{
    return part >= 0 && part < g->live_flag_cap && g->live_flag[part];
}

bool ub_game_part_visible(const ub_game *g, i32 part)
{
    if (part < 0 || part >= g->world.part_count) return false;
    const ub_part *p = &g->world.parts[part];
    if (p->transparency >= 250 || (p->flags & UB_PART_REMOVED)) return false;
    return in_workspace(g, ub_world_part_inst(&g->world, part));
}

void ub_game_part_pose(const ub_game *g, i32 part, vec3 *pos, mat3 *rot)
{
    if (g->phys_ok && ub_phys_part_pose(&g->phys, part, g->alpha, pos, rot)) return;
    *pos = g->world.parts[part].pos;
    *rot = g->world.parts[part].rot;
}

/* --------------------------------------------------------- the player */

static i32 find_root_by_class(const ub_world *w, const char *cls)
{
    for (i32 i = 0; i < w->count; ++i) {
        if (w->inst[i].parent >= 0 || !ub_world_alive(w, i)) continue;
        const char *a = ub_world_str(w, w->inst[i].class_name), *b = cls;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b && !(w->inst[i].flags & UB_INST_DETACHED)) return i;
    }
    return -1;
}

/* The camera's rules: StarterPlayer's, else Roblox's defaults. They are
 * the Player's from then on (scripts change them there). */
static void camera_rules(ub_game *g)
{
    ub_cam_state *c = &g->cam;
    memset(c, 0, sizeof *c);
    c->min_zoom = 0.5f;
    c->max_zoom = 128.0f;
    c->mouse_lock_ok = true;
    c->icon_enabled = true;
    c->camera_type = UB_CAMTYPE_CUSTOM;
    c->subject = -1;
    c->distance = 12.5f;
    const ub_world *w = &g->world;
    i32 sp = find_root_by_class(w, "StarterPlayer");
    if (sp < 0) return;
    const ub_value *v;
    if ((v = ub_world_prop_named(w, sp, "CameraMode")) && v->type == UB_PV_ENUM) c->camera_mode = v->v.e == 1;
    if ((v = ub_world_prop_named(w, sp, "CameraMinZoomDistance")) && v->type == UB_PV_FLOAT) c->min_zoom = v->v.f;
    if ((v = ub_world_prop_named(w, sp, "CameraMaxZoomDistance")) && v->type == UB_PV_FLOAT) c->max_zoom = v->v.f;
    if ((v = ub_world_prop_named(w, sp, "EnableMouseLockOption")) && v->type == UB_PV_BOOL) c->mouse_lock_ok = v->v.b;
    if ((v = ub_world_prop_named(w, sp, "DevCameraOcclusionMode")) && v->type == UB_PV_ENUM) c->occlusion = v->v.e == 1;
    ub_game_camera_limits(g);
}

void ub_game_camera_limits(ub_game *g)
{
    ub_cam_state *c = &g->cam;
    c->min_zoom = ub_clampf(c->min_zoom, 0.5f, 10000.0f);
    c->max_zoom = ub_clampf(c->max_zoom, c->min_zoom, 10000.0f);
}

static void make_player(ub_game *g)
{
    ub_world *w = &g->world;
    i32 ws = ub_world_workspace(w);
    g->players = find_root_by_class(w, "Players");
    if (g->players < 0) {
        g->players = ub_world_add(w, -1, UB_CLASS_OTHER, "Players", "Players");
        if (g->players >= 0) w->inst[g->players].flags |= UB_INST_RUNTIME;
    }
    g->player = g->players >= 0 ? ub_world_add(w, g->players, UB_CLASS_OTHER, "Player", "Player1") : -1;
    if (g->player >= 0) w->inst[g->player].flags |= UB_INST_RUNTIME;
    g->char_model = ub_world_add(w, ws, UB_CLASS_MODEL, NULL, "Player1");
    if (g->char_model >= 0) w->inst[g->char_model].flags |= UB_INST_RUNTIME;
    ub_part root;
    memset(&root, 0, sizeof root);
    root.pos = v3_add(g->spawn, v3(0, ROOT_HEIGHT, 0));
    root.rot = mat3_identity();
    root.size = v3(2, 2, 1);
    root.color = 0xA3A2A5;
    root.transparency = 255; /* invisible, never solid: the character body does the colliding */
    root.flags = UB_PART_ANCHORED;
    g->char_root = g->char_model >= 0
                       ? ub_world_add_part(w, g->char_model, UB_CLASS_PART, "Part", "HumanoidRootPart", &root)
                       : -1;
    g->humanoid = g->char_model >= 0 ? ub_world_add(w, g->char_model, UB_CLASS_OTHER, "Humanoid", "Humanoid") : -1;
    if (g->char_root >= 0) w->inst[g->char_root].flags |= UB_INST_RUNTIME;
    if (g->humanoid >= 0) w->inst[g->humanoid].flags |= UB_INST_RUNTIME;
    g->health = g->max_health = 100;
    g->walk_speed = UB_CHAR_WALK_SPEED;
    g->jump_height = UB_CHAR_JUMP_HEIGHT;
    /* where the player's GUI, tools and own scripts live */
    g->player_gui = g->backpack = g->player_scripts = -1;
    if (g->player >= 0) {
        g->player_gui = ub_world_add(w, g->player, UB_CLASS_OTHER, "PlayerGui", "PlayerGui");
        g->backpack = ub_world_add(w, g->player, UB_CLASS_FOLDER, "Backpack", "Backpack");
        g->player_scripts = ub_world_add(w, g->player, UB_CLASS_FOLDER, "PlayerScripts", "PlayerScripts");
        if (g->player_gui >= 0) ub_cdb_fill_defaults(w, g->player_gui, "PlayerGui");
    }
}

/* Is it a GUI to copy again after a respawn? (Everything but the
 * LayerCollectors that have ResetOnSpawn off.) */
static bool resets(const ub_world *w, i32 i)
{
    if (!ub_cdb_is_a(ub_world_str(w, w->inst[i].class_name), "LayerCollector")) return true;
    const ub_value *v = ub_world_prop_named(w, i, "ResetOnSpawn");
    return !v || v->type != UB_PV_BOOL || v->v.b;
}

int ub_game_fill_player_gui(ub_game *g, bool initial, i32 *added, int max)
{
    ub_world *w = &g->world;
    int n = 0;
    i32 sg = find_root_by_class(w, "StarterGui");
    if (g->player_gui < 0) return 0;
    /* clone first: the list of children must not change while walking it */
    i32 kids[1024];
    int nk = 0;
    for (i32 c = sg >= 0 ? w->inst[sg].first_child : -1; c >= 0 && nk < 1024; c = w->inst[c].next_sibling)
        if (ub_world_alive(w, c) && (initial || resets(w, c))) kids[nk++] = c;
    for (int k = 0; k < nk; ++k) {
        i32 copy = ub_world_clone(w, kids[k]);
        if (copy < 0) break;
        w->inst[copy].flags |= UB_INST_RUNTIME;
        ub_world_set_parent(w, copy, g->player_gui);
        if (n < max) added[n++] = copy;
    }
    if (initial) { /* StarterPlayerScripts: once, into PlayerScripts; StarterCharacterScripts: into the character */
        i32 sp = find_root_by_class(w, "StarterPlayer");
        for (i32 c = sp >= 0 ? w->inst[sp].first_child : -1; c >= 0; c = w->inst[c].next_sibling) {
            const char *cn = ub_world_str(w, w->inst[c].class_name);
            i32 dest = !ub_strcmp(cn, "StarterPlayerScripts") ? g->player_scripts
                     : !ub_strcmp(cn, "StarterCharacterScripts") ? g->char_model : -1;
            if (dest < 0) continue;
            i32 sk[256];
            int ns = 0;
            for (i32 q = w->inst[c].first_child; q >= 0 && ns < 256; q = w->inst[q].next_sibling)
                if (ub_world_alive(w, q)) sk[ns++] = q;
            for (int k = 0; k < ns; ++k) {
                i32 copy = ub_world_clone(w, sk[k]);
                if (copy < 0) break;
                w->inst[copy].flags |= UB_INST_RUNTIME;
                ub_world_set_parent(w, copy, dest);
                if (n < max) added[n++] = copy;
            }
        }
    }
    return n;
}

vec3 ub_game_root_position(const ub_game *g)
{
    return v3_add(g->phys_ok ? ub_char_feet(&g->chr, &g->phys, 1.0f) : g->spawn, v3(0, ROOT_HEIGHT, 0));
}

void ub_game_teleport(ub_game *g, vec3 feet)
{
    if (g->phys_ok) ub_char_teleport(&g->chr, &g->phys, feet);
    g->feet = feet;
    g->last_ground_part = -1;
    if (g->role == UB_ROLE_SERVER) {
        g->moved = true; /* the client moves it: tell it */
        g->report.have = false;
    }
    if (g->role == UB_ROLE_CLIENT && !g->moving_for_server) g->teleported = true; /* (a LocalScript did: say so) */
}

void ub_game_set_velocity(ub_game *g, vec3 v)
{
    if (g->phys_ok && !g->dead) ub_char_set_velocity(&g->chr, &g->phys, v);
    if (g->role == UB_ROLE_SERVER && !g->dead) {
        g->launch = v;
        g->launched = true;
    }
}

/* ------------------------------------------------------------- teams */

static bool bool_prop(const ub_world *w, i32 i, const char *k, bool dflt)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_BOOL ? v->v.b : dflt;
}

static i32 brick_prop(const ub_world *w, i32 i, const char *k)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && (v->type == UB_PV_BRICKCOLOR || v->type == UB_PV_INT) ? v->v.i : -1;
}

/* Where the player appears: their RespawnLocation, else a SpawnLocation
 * for their team (or a neutral one), else the place's first. */
void ub_game_pick_spawn(ub_game *g)
{
    ub_world *w = &g->world;
    if (g->respawn_location >= 0 && ub_world_alive(w, g->respawn_location)) {
        ub_game_set_respawn_location(g, g->respawn_location);
        return;
    }
    bool neutral = g->player < 0 || bool_prop(w, g->player, "Neutral", true);
    i32 color = g->player >= 0 ? brick_prop(w, g->player, "TeamColor") : -1;
    i32 cand[64], nc = 0, neutral_cand[64], nn = 0;
    i32 ws = ub_world_workspace(w);
    for (i32 i = 0; i < w->count; ++i) {
        if (w->inst[i].cls != UB_CLASS_SPAWN || w->inst[i].part < 0 || !ub_world_alive(w, i) ||
            !ub_world_is_descendant(w, i, ws) || !bool_prop(w, i, "Enabled", true))
            continue;
        bool sn = bool_prop(w, i, "Neutral", true);
        if (sn && nn < 64) neutral_cand[nn++] = i;
        if (!neutral && !sn && brick_prop(w, i, "TeamColor") == color && nc < 64) cand[nc++] = i;
    }
    static u32 seed = 12345;
    seed = seed * 1103515245u + 12345u;
    i32 pick = nc ? cand[(seed >> 16) % (u32)nc] : nn ? neutral_cand[(seed >> 16) % (u32)nn] : -1;
    if (pick >= 0) {
        const ub_part *p = &w->parts[w->inst[pick].part];
        g->spawn = v3_add(p->pos, v3_scale(p->rot.y, p->size.y * 0.5f));
    } else {
        g->spawn = g->spawn_default;
    }
}

/* At the start: into the AutoAssignable team with the fewest players (the
 * first, with one player). */
static void assign_team(ub_game *g)
{
    ub_world *w = &g->world;
    if (g->player < 0) return;
    i32 teams = find_root_by_class(w, "Teams");
    for (i32 c = teams >= 0 ? w->inst[teams].first_child : -1; c >= 0; c = w->inst[c].next_sibling) {
        if (!ub_world_alive(w, c) || ub_strcmp(ub_world_str(w, w->inst[c].class_name), "Team") != 0) continue;
        if (!bool_prop(w, c, "AutoAssignable", true)) continue;
        ub_value v;
        memset(&v, 0, sizeof v);
        v.type = UB_PV_REF;
        v.v.ref = c;
        ub_world_set_prop_named(w, g->player, "Team", &v);
        ub_value col;
        memset(&col, 0, sizeof col);
        col.type = UB_PV_BRICKCOLOR;
        col.v.i = brick_prop(w, c, "TeamColor") >= 0 ? brick_prop(w, c, "TeamColor") : 194;
        ub_world_set_prop_named(w, g->player, "TeamColor", &col);
        ub_value nv = ub_vbool(false);
        ub_world_set_prop_named(w, g->player, "Neutral", &nv);
        return;
    }
}

void ub_game_set_respawn_location(ub_game *g, i32 inst)
{
    ub_world *w = &g->world;
    if (inst >= 0 && ub_world_alive(w, inst) && w->inst[inst].part >= 0) {
        const ub_part *p = &w->parts[w->inst[inst].part];
        g->spawn = v3_add(p->pos, v3_scale(p->rot.y, p->size.y * 0.5f));
        g->respawn_location = inst;
    } else {
        g->spawn = g->spawn_default;
        g->respawn_location = -1;
    }
}

void ub_game_set_walk_speed(ub_game *g, f32 speed)
{
    g->walk_speed = ub_clampf(speed, 0, 1000);
    if (g->phys_ok && !g->dead) g->chr.walk_speed = g->walk_speed;
}

void ub_game_set_jump_height(ub_game *g, f32 height)
{
    g->jump_height = ub_clampf(height, 0, 1000);
    if (g->phys_ok) ub_char_set_jump_height(&g->chr, &g->phys, g->jump_height);
}

void ub_game_set_health(ub_game *g, f32 health)
{
    if (g->dead) return;
    g->health = ub_clampf(health, 0, g->max_health);
    if (g->health <= 0) {
        g->dead = true;
        g->respawn_timer = UB_GAME_RESPAWN_TIME;
        if (g->phys_ok) g->chr.walk_speed = 0;
        ub_info("the player died");
        if (g->role == UB_ROLE_CLIENT && g->link) { /* (a LocalScript killed it: the server decides, and respawns it) */
            u8 m = UB_MSG_RESET;
            g->link->send(g->link->ctx, true, &m, 1);
        }
        if (scr(g)) ub_script_died();
    }
}

static void respawn(ub_game *g)
{
    g->spawn_count++;
    g->dead = false;
    ub_game_pick_spawn(g);
    g->health = g->max_health;
    ub_game_teleport(g, g->spawn);
    if (g->phys_ok) g->chr.walk_speed = g->walk_speed;
    g->respawned = true;
    ub_info("the player respawned");
    /* the GUIs that reset on spawn are copied from StarterGui again (with
     * scripts running, the script engine does it, so events fire) */
    if (scr(g)) {
        ub_script_respawned();
    } else if (g->player_gui >= 0) {
        ub_world *w = &g->world;
        for (i32 c = w->inst[g->player_gui].first_child; c >= 0;) {
            i32 next = w->inst[c].next_sibling;
            if (resets(w, c)) ub_world_destroy(w, c);
            c = next;
        }
        i32 added[64];
        ub_game_fill_player_gui(g, false, added, 64);
    }
}

/* ------------------------------------------------------ script changes */

static bool part_watched(const ub_game *g, i32 part)
{
    return scr(g) && ub_script_watches_touch(ub_world_part_inst(&g->world, part));
}

void ub_game_part_changed(ub_game *g, i32 part, u32 what)
{
    if (part < 0 || part >= g->world.part_count) return;
    i32 inst = ub_world_part_inst(&g->world, part);
    if (inst == g->char_root) { /* moving the character's root moves the character */
        if (what & UB_CHANGE_PHYS) ub_game_teleport(g, v3_sub(g->world.parts[part].pos, v3(0, ROOT_HEIGHT, 0)));
        return;
    }
    make_live(g, part);
    if (what & UB_CHANGE_PHYS) g->constraints_dirty = true; /* (its body may have changed) */
    if ((what & UB_CHANGE_PHYS) && g->phys_ok)
        ub_phys_sync_part(&g->phys, part, in_workspace(g, inst), part_watched(g, part));
}

void ub_game_subtree_changed(ub_game *g, i32 inst)
{
    ub_world *w = &g->world;
    if (inst < 0 || inst >= w->count) return;
    g->constraints_dirty = true;
    /* walk the subtree (destroyed instances keep their children linked) */
    i32 cap = 64, sp = 0;
    i32 *stack = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    if (!stack) return;
    stack[sp++] = inst;
    while (sp > 0) {
        i32 i = stack[--sp];
        if (w->inst[i].part >= 0) ub_game_part_changed(g, w->inst[i].part, UB_CHANGE_LOOK | UB_CHANGE_PHYS);
        for (i32 c = w->inst[i].first_child; c >= 0; c = w->inst[c].next_sibling) {
            if (sp == cap && !grow((void **)&stack, &cap, cap + 1, sizeof(i32))) break;
            stack[sp++] = c;
        }
    }
    plat_free(stack);
    /* (parts that changed are drawn live from now on; make_live marks the
     * static set for rebuilding the first time) */
}

void ub_game_set_gravity(ub_game *g, f32 gravity)
{
    g->world.gravity = gravity;
    if (g->phys_ok) {
        ub_phys_set_gravity(&g->phys, gravity);
        ub_char_set_jump_height(&g->chr, &g->phys, g->jump_height);
    }
}

/* -------------------------------------------------------------- start */

static bool start(ub_game *g, ub_world *w, bool run_scripts, int role, const ub_game_link *link)
{
    memset(g, 0, sizeof *g);
    g->role = role;
    g->link = link;
    g->world = *w;
    ub_world_init(w); /* the game owns it now */
    ub_world *wd = &g->world;
    i32 ws = ub_world_workspace(wd);

    /* spawn: the first SpawnLocation, else the ground under the middle */
    i32 spawn = -1;
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    bool any = false;
    for (i32 i = 0; i < wd->count; ++i) {
        const ub_instance *in = &wd->inst[i];
        if (in->part < 0 || !ub_world_is_descendant(wd, i, ws)) continue;
        if (in->cls == UB_CLASS_SPAWN && spawn < 0) spawn = i;
        const ub_part *p = &wd->parts[in->part];
        lo = v3(UB_MIN(lo.x, p->pos.x), UB_MIN(lo.y, p->pos.y), UB_MIN(lo.z, p->pos.z));
        hi = v3(UB_MAX(hi.x, p->pos.x), UB_MAX(hi.y, p->pos.y), UB_MAX(hi.z, p->pos.z));
        any = true;
    }
    u64 t0 = plat_time_us();
    g->phys_ok = ub_phys_init(&g->phys, wd, NULL, 0);
    u32 build_ms = (u32)((plat_time_us() - t0) / 1000u);
    if (!g->phys_ok) ub_error("physics: out of memory building the world; nothing will move");
    if (spawn >= 0) {
        const ub_part *sp = &wd->parts[wd->inst[spawn].part];
        g->spawn = v3_add(sp->pos, v3_scale(sp->rot.y, sp->size.y * 0.5f));
    } else {
        vec3 c = any ? v3_scale(v3_add(lo, hi), 0.5f) : v3(0, 0, 0);
        ub_phys_hit h;
        g->spawn = g->phys_ok && ub_phys_raycast(&g->phys, v3(c.x, hi.y + 100.0f, c.z), v3(0, -1, 0), 1.0e5f, -1, &h)
                       ? h.point : v3(c.x, 0, c.z);
    }
    g->spawn_default = g->spawn;
    g->respawn_location = -1;
    /* (the player is made below; their team decides the spawn) */
    g->kill_height = UB_MIN(wd->fallen_height, g->spawn.y - 50.0f);
    g->feet = g->spawn;
    g->last_ground_part = -1;
    camera_rules(g);
    make_player(g);
    assign_team(g);
    ub_game_pick_spawn(g);
    g->feet = g->spawn;
    ub_rig_build(g);
    g->spawn_count = 1;
    g->constraints_dirty = true;
    {
        i32 added[64];
        ub_game_fill_player_gui(g, true, added, 64);
    }
    g->gui = ub_gui_new();
    /* the engine's own GUIs live under a CoreGui root scripts do not see into */
    g->core_gui = ub_world_add(&g->world, -1, UB_CLASS_FOLDER, "CoreGui", "CoreGui");
    if (g->core_gui >= 0) {
        g->world.inst[g->core_gui].flags |= UB_INST_RUNTIME;
        if (g->gui) ub_gui_set_core_root(g->gui, g->core_gui);
    }
    if (g->gui) ub_gui_set_root(g->gui, g->player_gui);
    if (g->phys_ok && !ub_char_init(&g->chr, &g->phys, g->spawn)) {
        ub_error("physics: out of memory adding the character");
        ub_phys_free(&g->phys);
        g->phys_ok = false;
    }
    /* parts that move are drawn every frame */
    for (i32 i = 0; i < wd->part_count && g->phys_ok; ++i)
        if (ub_phys_part_body(&g->phys, i) > 0) make_live(g, i);
    g->statics_dirty = true;

    if (g->phys_ok) {
        int loose = 0;
        for (i32 i = 0; i < g->phys.body_count; ++i) loose += g->phys.bodies[i].type == UB_BODY_DYNAMIC;
        ub_info("physics: %d loose part group(s), %d moving parts, %d welds, %d solid anchored parts, gravity %d, "
                "built in %u ms", loose, g->live_count, g->phys.stats.welds, g->phys.static_count,
                ub_roundf(wd->gravity), build_ms);
    }
    if (run_scripts) {
        g->scripts = ub_script_init(g);
        if (scr(g)) ub_script_start();
        else ub_error("scripts: could not start the script engine");
    }
    return true;
}

bool ub_game_start(ub_game *g, ub_world *w, bool run_scripts) { return start(g, w, run_scripts, UB_ROLE_SOLO, NULL); }

bool ub_game_start_server(ub_game *g, ub_world *w, bool run_scripts, const ub_game_link *link)
{
    return start(g, w, run_scripts, UB_ROLE_SERVER, link);
}

bool ub_game_start_client(ub_game *g, ub_world *w, const i32 ids[UB_HELLO_IDS], bool run_scripts,
                          const ub_game_link *link)
{
    memset(g, 0, sizeof *g);
    g->role = UB_ROLE_CLIENT;
    g->link = link;
    g->world = *w;
    ub_world_init(w);
    ub_world *wd = &g->world;
    g->player = ids[0];
    g->char_model = ids[1];
    g->char_root = ids[2];
    g->humanoid = ids[3];
    g->player_gui = ids[4];
    g->backpack = ids[5];
    g->player_scripts = ids[6];
    g->players = ids[7];
    g->health = g->max_health = 100;
    g->walk_speed = UB_CHAR_WALK_SPEED;
    g->jump_height = UB_CHAR_JUMP_HEIGHT;
    g->respawn_location = -1;
    g->spawn_count = 1;
    g->last_ground_part = -1;
    g->kill_height = -1.0e30f; /* (the server decides who fell out) */
    i32 rp = g->char_root >= 0 && ub_world_alive(wd, g->char_root) ? wd->inst[g->char_root].part : -1;
    g->spawn = g->spawn_default = rp >= 0 ? v3_sub(wd->parts[rp].pos, v3(0, ROOT_HEIGHT, 0)) : v3(0, 0, 0);
    g->feet = g->spawn;
    camera_rules(g);
    ub_rig_adopt(g);
    u64 t0 = plat_time_us();
    g->phys_ok = ub_phys_init(&g->phys, wd, NULL, -1); /* (the server moves the parts) */
    if (!g->phys_ok) ub_error("physics: out of memory building the world; nothing will move");
    g->constraints_dirty = true;
    g->gui = ub_gui_new();
    g->core_gui = ub_world_add(wd, -1, UB_CLASS_FOLDER, "CoreGui", "CoreGui");
    if (g->core_gui >= 0) {
        wd->inst[g->core_gui].flags |= UB_INST_RUNTIME;
        if (g->gui) ub_gui_set_core_root(g->gui, g->core_gui);
    }
    if (g->gui) ub_gui_set_root(g->gui, g->player_gui);
    if (g->phys_ok && !ub_char_init(&g->chr, &g->phys, g->spawn)) {
        ub_error("physics: out of memory adding the character");
        ub_phys_free(&g->phys);
        g->phys_ok = false;
    }
    for (i32 i = 0; i < wd->part_count && g->phys_ok; ++i)
        if (ub_phys_part_body(&g->phys, i) > 0) make_live(g, i);
    g->statics_dirty = true;
    ub_info("client: joined (%d instances, %d parts), physics built in %u ms", wd->count, wd->part_count,
            (u32)((plat_time_us() - t0) / 1000u));
    if (wd->journal) ub_world_journal_clear(wd); /* (all of it is new: nothing to report) */
    if (run_scripts) {
        g->scripts = ub_script_init(g);
        if (scr(g)) ub_script_start();
        else ub_error("scripts: could not start the script engine");
    }
    return true;
}

void ub_game_stop(ub_game *g)
{
    if (scr(g)) ub_script_shutdown();
    ub_gui_free(g->gui);
    if (g->phys_ok) ub_phys_free(&g->phys);
    plat_free(g->live_flag);
    plat_free(g->live);
    plat_free(g->carry);
    plat_free(g->movers);
    plat_free(g->interp);
    plat_free(g->interp_of);
    ub_world_free(&g->world);
    memset(g, 0, sizeof *g);
}

/* ---------------------------------------------------------- networked */

#define INTERP_DELAY 0.1f   /* the client shows the server's parts this far behind */

static void send_buf(ub_game *g, bool reliable, const ub_wbuf *b)
{
    if (g->link && !b->err) g->link->send(g->link->ctx, reliable, b->p, b->len);
}

static u32 fbits(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

/* server: the character's state and any move, to the client */
static void server_send(ub_game *g)
{
    u8 buf[64];
    ub_wbuf b;
    u32 sig = fbits(g->health) * 31u + fbits(g->max_health);
    sig = sig * 31u + fbits(g->walk_speed);
    sig = sig * 31u + fbits(g->jump_height);
    sig = sig * 31u + (g->dead ? 1u : 0u);
    sig = sig * 31u + g->spawn_count + 1u;
    if (sig != g->state_sig) {
        g->state_sig = sig;
        ub_wbuf_init(&b, buf, sizeof buf);
        ub_w_u8(&b, UB_MSG_STATE);
        ub_w_f32(&b, g->health);
        ub_w_f32(&b, g->max_health);
        ub_w_f32(&b, g->walk_speed);
        ub_w_f32(&b, g->jump_height);
        ub_w_u8(&b, g->dead ? 1 : 0);
        ub_w_varu(&b, g->spawn_count);
        send_buf(g, true, &b);
    }
    if (g->moved || g->launched) {
        if (g->moved) g->move_seq++;
        ub_wbuf_init(&b, buf, sizeof buf);
        ub_w_u8(&b, UB_MSG_MOVE);
        ub_w_u16(&b, g->move_seq);
        ub_w_u8(&b, (u8)((g->moved ? 1 : 0) | (g->launched ? 2 : 0)));
        ub_w_vec3(&b, g->phys_ok ? ub_char_feet(&g->chr, &g->phys, 1.0f) : g->feet);
        ub_w_vec3(&b, g->launch);
        send_buf(g, true, &b);
        g->moved = g->launched = false;
    }
}

/* server, each step: the character goes where the client says it is */
static void follow_report(ub_game *g)
{
    ub_report *r = &g->report;
    if (!r->have || !g->phys_ok) return;
    f32 ahead = ub_clampf(g->time - r->time, 0, 0.25f);
    vec3 target = v3_add(r->feet, v3_scale(r->vel, ahead));
    vec3 cur = ub_char_feet(&g->chr, &g->phys, 1.0f);
    vec3 d = v3_sub(target, cur);
    if (v3_length(d) > 6.0f) {
        ub_char_teleport(&g->chr, &g->phys, target);
        return;
    }
    /* by velocity, so it pushes what it walks into as it did on the client */
    vec3 v = v3_scale(d, 1.0f / UB_PHYS_DT);
    f32 cap = UB_MAX(v3_length(r->vel) + 20.0f, 40.0f), vl = v3_length(v);
    if (vl > cap) v = v3_scale(v, cap / vl);
    g->phys.bodies[g->chr.body].v = v;
    ub_phys_wake(&g->phys, g->chr.body);
}

static u16 angle16(f32 a) { return (u16)(i32)ub_roundf(ub_wrap_angle(a) * (32767.0f / UB_PI)); }
static f32 angle_of16(u16 v) { return (f32)(i16)v * (UB_PI / 32767.0f); }

/* client: where the character is, 20 times a second */
static void client_report(ub_game *g, f32 dt)
{
    g->report_timer -= dt;
    g->report_idle += dt;
    if (g->report_timer > 0 || !g->phys_ok) return;
    g->report_timer = g->report_timer < -0.05f ? 1.0f / UB_REPORT_HZ : g->report_timer + 1.0f / UB_REPORT_HZ;
    u8 buf[64];
    ub_wbuf b;
    ub_wbuf_init(&b, buf, sizeof buf);
    ub_w_u8(&b, UB_MSG_REPORT);
    ub_w_u16(&b, g->move_seq);
    ub_w_f32(&b, g->time);
    ub_w_vec3(&b, ub_char_feet(&g->chr, &g->phys, 1.0f));
    ub_w_vec3(&b, ub_char_velocity(&g->chr, &g->phys));
    ub_w_u16(&b, angle16(g->pose_facing));
    ub_w_u16(&b, angle16(g->pose_phase));
    ub_w_u8(&b, (u8)ub_roundf(ub_clampf(g->pose_amount, 0, 1) * 255.0f));
    ub_w_u8(&b, (u8)ub_roundf(ub_clampf(g->pose_fallen, 0, 1) * 255.0f));
    ub_w_u8(&b, g->teleported ? 1 : 0);
    /* standing still, nothing changed: 4 a second do (the time is not compared) */
    bool same = b.len == g->report_len && !memcmp(b.p + 7, g->report_last + 7, b.len - 7) &&
                !memcmp(b.p + 1, g->report_last + 1, 2);
    if (same && g->report_idle < 0.25f) return;
    g->report_idle = 0;
    g->teleported = false;
    memcpy(g->report_last, b.p, b.len);
    g->report_len = b.len;
    send_buf(g, false, &b);
}

static bool finite3(vec3 v)
{
    return v.x == v.x && v.y == v.y && v.z == v.z && ub_absf(v.x) < 1e7f && ub_absf(v.y) < 1e7f && ub_absf(v.z) < 1e7f;
}

static void got_report(ub_game *g, ub_rbuf *r)
{
    ub_report nr = g->report;
    nr.move_seq = ub_r_u16(r);
    nr.client_time = ub_r_f32(r);
    nr.feet = ub_r_vec3(r);
    nr.vel = ub_r_vec3(r);
    nr.facing = angle_of16(ub_r_u16(r));
    nr.phase = angle_of16(ub_r_u16(r));
    nr.amount = (f32)ub_r_u8(r) / 255.0f;
    nr.fallen = (f32)ub_r_u8(r) / 255.0f;
    bool jumped_there = (ub_r_u8(r) & 1) != 0; /* a LocalScript teleported it (Roblox allows that) */
    if (r->err || !finite3(nr.feet) || !finite3(nr.vel)) return;
    if ((i16)(u16)(nr.move_seq - g->move_seq) < 0) return; /* from before the server last moved it */
    ub_report *o = &g->report;
    if (o->have && !(nr.client_time > o->client_time)) return; /* (an old one, overtaken) */
    if (o->have && !g->dead && !jumped_there) { /* too fast to be walking, jumping or falling? */
        f32 dt = ub_clampf(nr.client_time - o->client_time, 0, 1.0f);
        vec3 d = v3_sub(nr.feet, o->feet);
        f32 h = ub_sqrtf(d.x * d.x + d.z * d.z);
        f32 hmax = (UB_MAX(g->walk_speed, UB_CHAR_WALK_SPEED) * 3.0f + 30.0f) * dt + 4.0f;
        f32 upmax = (g->chr.jump_speed * 2.0f + 60.0f) * dt + 4.0f;
        if (h > hmax || d.y > upmax) {
            o->rejected++;
            o->have = false;
            g->moved = true; /* back to where the server has it */
            return;
        }
    }
    nr.have = true;
    nr.time = g->time;
    nr.rejected = o->rejected;
    *o = nr;
    g->pose_from_app = true;
    g->pose_facing = nr.facing;
    g->pose_phase = nr.phase;
    g->pose_amount = nr.amount;
    g->pose_fallen = nr.fallen;
}

/* client: the server's word on the character */
static void got_state(ub_game *g, ub_rbuf *r)
{
    f32 health = ub_r_f32(r), maxh = ub_r_f32(r), ws = ub_r_f32(r), jh = ub_r_f32(r);
    bool dead = ub_r_u8(r) != 0;
    u32 spawns = ub_r_varu(r);
    if (r->err) return;
    g->max_health = maxh;
    if (ws != g->walk_speed) ub_game_set_walk_speed(g, ws);
    if (jh != g->jump_height) ub_game_set_jump_height(g, jh);
    if (spawns != g->spawn_count && !dead) { /* came back */
        g->spawn_count = spawns;
        g->dead = false;
        g->health = health;
        if (g->phys_ok) g->chr.walk_speed = g->walk_speed;
        g->respawned = true;
        if (scr(g)) ub_script_respawned();
    } else if (dead && !g->dead) {
        g->dead = true;
        g->health = 0;
        g->respawn_timer = UB_GAME_RESPAWN_TIME;
        if (g->phys_ok) g->chr.walk_speed = 0;
        if (scr(g)) ub_script_died();
    } else if (!g->dead) {
        g->health = health;
    }
}

static void got_move(ub_game *g, ub_rbuf *r)
{
    u16 seq = ub_r_u16(r);
    u8 flags = ub_r_u8(r);
    vec3 feet = ub_r_vec3(r), vel = ub_r_vec3(r);
    if (r->err) return;
    if (flags & 1) {
        g->move_seq = seq;
        g->moving_for_server = true;
        ub_game_teleport(g, feet);
        g->moving_for_server = false;
    }
    if ((flags & 2) && g->phys_ok) ub_char_set_velocity(&g->chr, &g->phys, vel);
}

void ub_game_message(ub_game *g, const u8 *msg, u32 len)
{
    ub_rbuf r;
    ub_rbuf_init(&r, msg, len);
    u8 type = ub_r_u8(&r);
    bool server = g->role == UB_ROLE_SERVER, client = g->role == UB_ROLE_CLIENT;
    switch (type) {
    case UB_MSG_REPORT: if (server) got_report(g, &r); break;
    case UB_MSG_STATE: if (client) got_state(g, &r); break;
    case UB_MSG_MOVE: if (client) got_move(g, &r); break;
    case UB_MSG_CHAT:
        if (server) {
            u32 n;
            const char *t = ub_r_str(&r, &n);
            char text[UB_HUD_TEXT + 1];
            if (!t || n > UB_HUD_TEXT) break;
            memcpy(text, t, n);
            text[n] = 0;
            ub_game_player_chat(g, text);
        }
        break;
    case UB_MSG_RESET:
        if (server && !g->dead) ub_game_set_health(g, 0);
        break;
    case UB_MSG_SCRIPT:
        if (scr(g)) ub_script_message(msg, len);
        break;
    default: break;
    }
}

u32 ub_game_hello(const ub_game *g, u8 *out, u32 cap)
{
    ub_wbuf b;
    ub_wbuf_init(&b, out, cap);
    ub_w_u8(&b, UB_MSG_HELLO);
    const i32 ids[UB_HELLO_IDS] = { g->player, g->char_model, g->char_root, g->humanoid,
                                    g->player_gui, g->backpack, g->player_scripts, g->players };
    for (int i = 0; i < UB_HELLO_IDS; ++i) ub_w_vari(&b, ids[i]);
    return b.err ? 0 : b.len;
}

/* --- the client's view of the parts the server moves */

static ub_interp *interp_for(ub_game *g, i32 part, bool make)
{
    if (part < g->interp_of_cap && g->interp_of[part] >= 0) return &g->interp[g->interp_of[part]];
    if (!make) return NULL;
    if (part >= g->interp_of_cap) {
        i32 old = g->interp_of_cap;
        if (!grow((void **)&g->interp_of, &g->interp_of_cap, part + 1, sizeof(i32))) return NULL;
        for (i32 i = old; i < g->interp_of_cap; ++i) g->interp_of[i] = -1;
    }
    if (!grow((void **)&g->interp, &g->interp_cap, g->interp_count + 1, sizeof(ub_interp))) return NULL;
    g->interp_of[part] = g->interp_count;
    ub_interp *e = &g->interp[g->interp_count++];
    memset(e, 0, sizeof *e);
    e->part = part;
    return e;
}

static void interp_drop(ub_game *g, i32 part)
{
    if (part < 0 || part >= g->interp_of_cap || g->interp_of[part] < 0) return;
    i32 k = g->interp_of[part];
    g->interp_of[part] = -1;
    g->interp[k] = g->interp[--g->interp_count];
    if (k < g->interp_count) g->interp_of[g->interp[k].part] = k;
}

void ub_game_part_from_server(ub_game *g, i32 inst, int what, u32 tick)
{
    ub_world *w = &g->world;
    i32 pi = inst >= 0 && inst < w->count ? w->inst[inst].part : -1;
    if (pi < 0) return;
    if (what == 3) { /* its looks */
        ub_game_part_changed(g, pi, UB_CHANGE_LOOK);
        return;
    }
    if (what == 0) { /* all of it: as it is now */
        interp_drop(g, pi);
        ub_game_part_changed(g, pi, UB_CHANGE_LOOK | UB_CHANGE_PHYS);
        return;
    }
    f32 ts = (f32)tick * UB_REPL_TICK_DT;
    f32 off = ts - g->time;
    if (!g->clock_ok || off > g->clock_off) {
        g->clock_off = off;
        g->clock_ok = true;
    }
    const ub_part *p = &w->parts[pi];
    ub_interp *e = interp_for(g, pi, false);
    if (!e) {
        e = interp_for(g, pi, true);
        if (!e) {
            ub_game_part_changed(g, pi, UB_CHANGE_PHYS);
            return;
        }
        e->p0 = e->p1 = p->pos;
        ub_mat3_to_quat(&p->rot, e->q0);
        memcpy(e->q1, e->q0, sizeof e->q0);
        e->t0 = e->t1 = ts;
        return;
    }
    if (!(ts > e->t1)) return;
    e->p0 = e->p1;
    memcpy(e->q0, e->q1, sizeof e->q0);
    e->t0 = e->t1;
    e->p1 = p->pos;
    ub_mat3_to_quat(&p->rot, e->q1);
    e->t1 = ts;
}

static void interp_apply(ub_game *g, f32 dt)
{
    ub_world *w = &g->world;
    g->clock_off -= 0.02f * dt; /* (drifts down until a sample comes sooner) */
    f32 rt = g->time + g->clock_off - INTERP_DELAY;
    for (i32 k = 0; k < g->interp_count;) {
        ub_interp *e = &g->interp[k];
        i32 pi = e->part;
        if (pi >= w->part_count || (w->parts[pi].flags & UB_PART_REMOVED)) {
            interp_drop(g, pi);
            continue;
        }
        f32 a = e->t1 > e->t0 ? ub_clampf((rt - e->t0) / (e->t1 - e->t0), 0, 1) : 1.0f;
        f32 q[4], sgn = 1.0f, n = 0;
        if (e->q0[0] * e->q1[0] + e->q0[1] * e->q1[1] + e->q0[2] * e->q1[2] + e->q0[3] * e->q1[3] < 0) sgn = -1.0f;
        for (int c = 0; c < 4; ++c) {
            q[c] = e->q0[c] * (1 - a) + e->q1[c] * sgn * a;
            n += q[c] * q[c];
        }
        n = n > 0 ? 1.0f / ub_sqrtf(n) : 0;
        for (int c = 0; c < 4; ++c) q[c] *= n;
        ub_part *p = &w->parts[pi];
        p->pos = v3_add(e->p0, v3_scale(v3_sub(e->p1, e->p0), a));
        p->rot = ub_quat_to_mat3(q);
        ub_game_part_changed(g, pi, UB_CHANGE_PHYS);
        if (a >= 1.0f && rt > e->t1 + 1.0f) { /* at rest where the server left it */
            interp_drop(g, pi);
            continue;
        }
        ++k;
    }
}

void ub_game_destroy_from_server(ub_game *g, i32 inst)
{
    ub_world *w = &g->world;
    if (!ub_world_alive(w, inst)) return;
    i32 n = 0;
    i32 *sub = ub_world_subtree(w, inst, &n);
    for (i32 k = 0; sub && k < n; ++k) {
        if (w->inst[sub[k]].part >= 0) interp_drop(g, w->inst[sub[k]].part);
        ub_game_uncarry(g, sub[k], sub[k]);
        ub_game_uncarry(g, sub[k], -1);
    }
    if (scr(g)) {
        ub_script_destroy_from_server(inst); /* (its slots are reclaimed once scripts let go of them) */
        ub_game_subtree_changed(g, inst);
    } else {
        ub_world_destroy(w, inst);
        ub_game_subtree_changed(g, inst);
        for (i32 k = n - 1; sub && k >= 0; --k) ub_world_reclaim(w, sub[k]);
    }
    plat_free(sub);
}

void ub_game_after_replication(ub_game *g)
{
    ub_world *w = &g->world;
    ub_journal *j = w->journal;
    if (!j) return;
    for (i32 k = 0; k < j->n; ++k) {
        i32 i = j->list[k];
        if (i < 0 || i >= w->count || !ub_world_alive(w, i)) continue;
        u8 f = j->flags[i];
        if (f & (UB_JN_NEW | UB_JN_TREE)) ub_game_subtree_changed(g, i);
        if ((f & (UB_JN_NEW | UB_JN_PROPS | UB_JN_KEYS)) && ub_constraint_class(ub_world_str(w, w->inst[i].class_name)))
            g->constraints_dirty = true;
    }
    if (scr(g)) ub_script_replicated(); /* scripts start, joints join, signals fire */
    ub_world_journal_clear(w);
}

/* -------------------------------------------------------------- frame */

static void dispatch_touches(ub_game *g)
{
    for (i32 i = 0; i < g->phys.touch_count && scr(g); ++i) {
        i32 a = g->phys.touches[i].a, b = g->phys.touches[i].b;
        i32 ia = a >= 0 ? ub_world_part_inst(&g->world, a) : g->char_root;
        i32 ib = b >= 0 ? ub_world_part_inst(&g->world, b) : g->char_root;
        if (ia >= 0 && ib >= 0) ub_script_touched(ia, ib);
    }
    /* the character's legs are rays: standing on something is touching it */
    i32 gp = g->chr.ground_part;
    if (gp != g->last_ground_part) {
        g->last_ground_part = gp;
        if (gp >= 0 && scr(g)) ub_script_touched(ub_world_part_inst(&g->world, gp), g->char_root);
    }
}

void ub_game_update(ub_game *g, f32 dt, vec3 move, bool jump)
{
    u64 s0 = plat_time_us();
    if (scr(g)) ub_script_begin_frame(dt);
    u64 p0 = plat_time_us();
    if (g->dead) {
        move = v3(0, 0, 0);
        jump = false;
        g->respawn_timer -= dt;
        if (g->respawn_timer <= 0 && g->role != UB_ROLE_CLIENT) respawn(g); /* (a client's comes from the server) */
    }
    if (g->role == UB_ROLE_CLIENT) interp_apply(g, dt);
    g->alpha = 1.0f;
    u32 script_in_steps = 0;
    if (g->phys_ok) {
        g->accum += dt;
        int steps = 0;
        while (g->accum >= UB_PHYS_DT && steps < UB_GAME_MAX_STEPS) {
            g->time += UB_PHYS_DT;
            ub_constraints_step(g);
            if (g->role == UB_ROLE_SERVER) follow_report(g);
            else ub_char_before_step(&g->chr, &g->phys, move, jump);
            ub_phys_step(&g->phys);
            ub_char_after_step(&g->chr, &g->phys);
            g->accum -= UB_PHYS_DT;
            steps++;
            /* the character's root part follows it */
            if (g->char_root >= 0)
                g->world.parts[g->world.inst[g->char_root].part].pos = ub_game_root_position(g);
            u64 t = plat_time_us();
            dispatch_touches(g);
            script_in_steps += (u32)(plat_time_us() - t);
            if (g->chr.jumped) {
                g->chr.jumped = false;
                g->jumps++;
                g->jumped = true;
                ub_debug("jump %u", g->jumps);
            }
            if (g->chr.landed_speed > 0) {
                g->landed_speed = UB_MAX(g->landed_speed, g->chr.landed_speed);
                g->chr.landed_speed = 0;
            }
            vec3 feet = ub_char_feet(&g->chr, &g->phys, 1.0f);
            if (!g->dead && g->role != UB_ROLE_CLIENT && (feet.y < g->kill_height || !(feet.y == feet.y))) {
                ub_info("fell out of the world");
                if (scr(g)) {
                    ub_game_set_health(g, 0); /* dies, comes back after a moment */
                    ub_char_teleport(&g->chr, &g->phys, g->spawn);
                    if (g->role == UB_ROLE_SERVER) {
                        g->moved = true;
                        g->report.have = false;
                    }
                } else {
                    respawn(g);
                }
            }
        }
        if (steps == UB_GAME_MAX_STEPS) g->accum = 0; /* too slow to keep up: slow down instead */
        g->alpha = g->accum / UB_PHYS_DT;
        g->feet = ub_char_feet(&g->chr, &g->phys, g->alpha);
        if (!g->rest_logged && g->phys.stats.awake == 0 && g->phys.stats.steps > 2) {
            g->rest_logged = true;
            ub_info("physics: all loose parts at rest after %u.%02u s", (u32)g->time, (u32)(g->time * 100.0f) % 100u);
        }
    } else {
        g->time += dt;
    }
    ub_rig_pose(g, dt);
    u64 p1 = plat_time_us();
    if (scr(g)) ub_script_end_frame(dt);
    if (g->role == UB_ROLE_SERVER) server_send(g);
    if (g->role == UB_ROLE_CLIENT) client_report(g, dt);
    u64 s1 = plat_time_us();
    g->phys_us = (u32)(p1 - p0) - script_in_steps;
    g->script_us = (u32)(s1 - s0) - g->phys_us;
}

/* ------------------------------------------------------------ the HUD */

static void copy_text(char *d, int cap, const char *s)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; ++i) d[i] = s[i] == '\n' || s[i] == '\r' ? ' ' : s[i];
    d[i] = 0;
}

void ub_hud_chat(ub_game *g, const char *who, const char *text, u32 color)
{
    ub_chat_line *l = &g->hud.chat[g->hud.chat_count % UB_HUD_CHAT_LINES];
    copy_text(l->who, (int)sizeof l->who, who ? who : "");
    copy_text(l->text, (int)sizeof l->text, text);
    l->color = color;
    l->time = g->time;
    g->hud.chat_count++;
}

void ub_hud_notify(ub_game *g, const char *title, const char *text, f32 duration)
{
    ub_notice *n = &g->hud.notices[g->hud.notice_count % 8];
    copy_text(n->title, (int)sizeof n->title, title);
    copy_text(n->text, (int)sizeof n->text, text);
    n->time = g->time;
    n->duration = duration > 0 ? duration : 5;
    g->hud.notice_count++;
}

void ub_hud_bubble(ub_game *g, i32 adornee, const char *text)
{
    ub_bubble *b = &g->hud.bubbles[g->hud.bubble_count % 16];
    b->adornee = adornee;
    copy_text(b->text, (int)sizeof b->text, text);
    b->time = g->time;
    g->hud.bubble_count++;
}

u32 ub_hud_name_color(const char *name)
{
    static const u32 k[8] = { 0xFD5A6E, 0x3DB4FF, 0x34C77B, 0xB97FD0, 0xF5A030, 0xF5D547, 0xF0B8CC, 0xD9C9A0 };
    u32 h = 0;
    for (const char *p = name; *p; ++p) h = h * 31u + (u8)*p;
    return k[h % 8];
}

void ub_game_player_chat(ub_game *g, const char *text)
{
    if (!text || !text[0]) return;
    if (g->role == UB_ROLE_CLIENT && g->link) { /* the server hears it too (its Chatted) */
        u8 buf[UB_HUD_TEXT + 16];
        ub_wbuf b;
        ub_wbuf_init(&b, buf, sizeof buf);
        ub_w_u8(&b, UB_MSG_CHAT);
        u32 n = (u32)ub_strlen(text);
        ub_w_str(&b, text, n < UB_HUD_TEXT ? n : UB_HUD_TEXT);
        if (!b.err) g->link->send(g->link->ctx, true, b.p, b.len);
    }
    const char *name = g->player >= 0 ? ub_world_str(&g->world, g->world.inst[g->player].name) : "Player";
    ub_hud_chat(g, name, text, ub_hud_name_color(name));
    if (g->char_model >= 0 && ub_world_alive(&g->world, g->char_model)) {
        i32 head = ub_world_find_child(&g->world, g->char_model, "Head");
        if (head >= 0) ub_hud_bubble(g, head, text);
    }
    if (scr(g)) ub_script_chatted(text); /* Player.Chatted */
}

void ub_game_reset_character(ub_game *g)
{
    if (g->hud.reset_off || g->dead) return;
    g->hud.reset_requested = true;
    if (g->role == UB_ROLE_CLIENT) { /* the server decides */
        u8 m = UB_MSG_RESET;
        if (g->link) g->link->send(g->link->ctx, true, &m, 1);
        return;
    }
    ub_game_set_health(g, 0);
}

bool ub_game_scripts(const ub_game *g) { return scr(g); }
