/* Uniblox - playing a place on screen (see app_play.h). */
#include "app/app_play.h"
#include "gui/ub_gui.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "crt/ub_crt.h"

#define TURN_SPEED 1.8f        /* radians/s for arrow keys */
#define LOOK_SENSITIVITY 0.006f

/* A part as the renderer wants it. */
ub_part_draw play_part_draw(const ub_world *w, const ub_part *p)
{
    ub_part_draw d;
    memset(&d, 0, sizeof d);
    u16 id = p->look ? p->look : p->mesh;
    d.mesh = id ? ub_world_render_mesh(w, id) : NULL;
    d.pos = p->pos;
    d.rot = p->rot;
    d.size = p->size;
    d.color = p->color;
    d.shape = p->shape;
    d.material = p->material;
    d.flags = (u8)(((p->flags & UB_PART_GLOW) ? UB_DRAW_GLOW : 0) | ub_draw_see_through(p->transparency));
    return d;
}

/* Roblox-style lighting -> renderer environment: sun position from the
 * clock time and latitude (equinox), fog from FogEnd. */
ub_render_env play_env(const ub_world *w)
{
    ub_render_env e = ub_render_default_env();
    const ub_lighting *l = &w->lighting;
    f32 h = (l->clock_time - 12.0f) * (UB_PI / 12.0f);        /* hour angle */
    f32 lat = l->latitude * (UB_PI / 180.0f);
    /* east = +X, north = +Z here (Roblox's north is -Z; Z is flipped) */
    vec3 sun = v3(-ub_sinf(h), ub_cosf(lat) * ub_cosf(h), -ub_sinf(lat) * ub_cosf(h));
    f32 day = ub_clampf((sun.y + 0.05f) / 0.25f, 0.0f, 1.0f);  /* fades out at dusk */
    if (sun.y < 0.12f) sun.y = 0.12f;                           /* keep some modelling at night */
    e.sun_dir = v3_normalize(sun);
    f32 b = ub_clampf(l->brightness / 2.0f, 0.4f, 1.5f) * (0.15f + 0.85f * day);
    e.sun_strength *= b;
    u32 sc = e.sun_color;
    u32 r = (u32)UB_MIN(255.0f, (f32)((sc >> 16) & 255) * b), g = (u32)UB_MIN(255.0f, (f32)((sc >> 8) & 255) * b),
        bl = (u32)UB_MIN(255.0f, (f32)(sc & 255) * b);
    e.sun_color = (r << 16) | (g << 8) | bl;
    if (day < 1.0f) { /* dusk/night sky */
        e.sky_top = 0x0B1630;
        e.sky_horizon = day > 0.3f ? 0xE0905A : 0x1C2A48;
    }
    e.fog_density = l->fog_end > 1.0f && l->fog_end < 20000.0f ? 3.0f / l->fog_end : 0.0012f;
    /* an Atmosphere in Lighting (8C): its Color tints the horizon, Decay the
     * sky above, Density the haze */
    for (i32 i = 0; i < w->count; ++i) {
        if (w->inst[i].parent < 0 || !ub_world_alive(w, i)) continue;
        if (ub_strcmp(ub_world_str(w, w->inst[i].class_name), "Atmosphere") != 0) continue;
        i32 par = w->inst[i].parent;
        if (w->inst[par].cls != UB_CLASS_LIGHTING) continue;
        const ub_value *c = ub_world_prop_named(w, i, "Color"), *dc = ub_world_prop_named(w, i, "Decay");
        const ub_value *dn = ub_world_prop_named(w, i, "Density");
        if (c && c->type == UB_PV_COLOR3 && day > 0.3f)
            e.sky_horizon = ((u32)(ub_clampf(c->v.c3.r, 0, 1) * 255) << 16) | ((u32)(ub_clampf(c->v.c3.g, 0, 1) * 255) << 8) |
                            (u32)(ub_clampf(c->v.c3.b, 0, 1) * 255);
        if (dc && dc->type == UB_PV_COLOR3 && day > 0.3f) {
            u32 t = e.sky_top;
            f32 k = 0.35f;
            u32 r2 = (u32)((f32)((t >> 16) & 255) * (1 - k) + ub_clampf(dc->v.c3.r, 0, 1) * 255 * k);
            u32 g2 = (u32)((f32)((t >> 8) & 255) * (1 - k) + ub_clampf(dc->v.c3.g, 0, 1) * 255 * k);
            u32 b2 = (u32)((f32)(t & 255) * (1 - k) + ub_clampf(dc->v.c3.b, 0, 1) * 255 * k);
            e.sky_top = (r2 << 16) | (g2 << 8) | b2;
        }
        if (dn && dn->type == UB_PV_FLOAT && dn->v.f > 0) {
            f32 dens = ub_clampf(dn->v.f, 0, 1) * 0.02f;
            if (dens > e.fog_density) e.fog_density = dens;
        }
        break;
    }
    return e;
}

/* A simple blocky figure: torso, round head, arms and legs that swing
 * while walking. Each limb pivots at its top (shoulder or hip). */
static void draw_limb(vec3 base, const mat3 *body, vec3 pivot, vec3 size, f32 swing, u32 color,
                      ub_material mat)
{
    mat3 r = mat3_from_euler(0, swing, 0);
    mat3 rot;   /* body rotation followed by the swing about the pivot */
    rot.x = mat3_apply(body, r.x);
    rot.y = mat3_apply(body, r.y);
    rot.z = mat3_apply(body, r.z);
    vec3 hang = mat3_apply(&rot, v3(0, -size.y * 0.5f, 0));
    ub_part_draw p;
    memset(&p, 0, sizeof p);
    p.pos = v3_add(v3_add(base, mat3_apply(body, pivot)), hang);
    p.rot = rot;
    p.size = size;
    p.color = color;
    p.shape = UB_SHAPE_BOX;
    p.material = (u8)mat;
    p.flags = 0;
    ub_render_part(&p);
}

void play_draw_character(f32 x, f32 y, f32 z, f32 facing, f32 phase, f32 amount, f32 fallen)
{
    const u32 shirt = 0x2F8F9D, pants = 0x3B3F4A, skin = 0xF0C8A0;
    mat3 body = mat3_from_euler(facing, -UB_HALF_PI * fallen, 0); /* dead: tipped over backwards */
    vec3 base = v3(x, y, z);
    f32 swing = ub_sinf(phase) * 0.7f * amount;

    draw_limb(base, &body, v3(-0.5f, 2.0f, 0), v3(0.9f, 2.0f, 1.0f), swing, pants, UB_MAT_PLASTIC);
    draw_limb(base, &body, v3(0.5f, 2.0f, 0), v3(0.9f, 2.0f, 1.0f), -swing, pants, UB_MAT_PLASTIC);
    ub_part_draw torso;
    memset(&torso, 0, sizeof torso);
    torso.pos = v3_add(base, mat3_apply(&body, v3(0, 3.0f, 0)));
    torso.rot = body;
    torso.size = v3(2.0f, 2.0f, 1.0f);
    torso.color = shirt;
    torso.shape = UB_SHAPE_BOX;
    torso.material = UB_MAT_PLASTIC;
    torso.flags = 0;
    ub_render_part(&torso);
    draw_limb(base, &body, v3(-1.5f, 4.0f, 0), v3(0.9f, 2.0f, 0.9f), -swing, skin, UB_MAT_PLASTIC);
    draw_limb(base, &body, v3(1.5f, 4.0f, 0), v3(0.9f, 2.0f, 0.9f), swing, skin, UB_MAT_PLASTIC);
    ub_part_draw head = torso;
    head.pos = v3_add(base, mat3_apply(&body, v3(0, 4.75f, 0)));
    head.size = v3(1.4f, 1.4f, 1.4f);
    head.color = skin;
    head.shape = UB_SHAPE_BALL;
    ub_render_part(&head);
}

void play_view_init(ub_play_view *v)
{
    memset(v, 0, sizeof *v);
    v->pitch = -0.35f;
    v->zoom = v->zoom_now = v->cam_len = 12.5f;
    v->look_scale = 1.0f;
    v->fov_deg = 70.0f;
    v->shift_lock_allowed = true;
}

void play_view_free(ub_play_view *v)
{
    plat_free(v->statics);
    v->statics = NULL;
    v->static_count = v->static_cap = 0;
}

static void rebuild_statics(ub_play_view *v, ub_game *g)
{
    const ub_world *w = &g->world;
    if (v->static_cap < w->part_count + 1) {
        plat_free(v->statics);
        v->static_cap = w->part_count + 1024;
        v->statics = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * (size_t)v->static_cap);
        if (!v->statics) v->static_cap = 0;
    }
    v->static_count = 0;
    for (i32 i = 0; i < w->part_count && v->statics; ++i)
        if (ub_game_part_visible(g, i) && !ub_game_part_live(g, i)) v->statics[v->static_count++] = play_part_draw(w, &w->parts[i]);
    ub_render_set_static(v->statics, v->static_count);
    g->statics_dirty = false;
}

void play_view_reset(ub_play_view *v, ub_game *g)
{
    v->death_t = 0;
    v->walk_amount = 0;
    v->have_cam = false;
    v->cam_len = v->zoom_now = v->zoom;
    rebuild_statics(v, g);
}

/* Is a part's instance the character's body (not a tool it holds)? */
static bool body_part(const ub_game *g, i32 inst)
{
    const ub_world *w = &g->world;
    if (g->char_model < 0) return false;
    for (i32 a = inst; a >= 0; a = w->inst[a].parent) {
        if (a == g->char_model) return true;
        if (!ub_strcmp(ub_world_str(w, w->inst[a].class_name), "Tool")) return false;
    }
    return false;
}

/* ------------------------------------------------------------- camera */

#define HEAD_ABOVE_FEET 4.5f   /* the camera's subject: HumanoidRootPart + 1.5 studs, as Roblox's */
#define SNAP_IN 2.0f           /* zooming in closer than this goes into first person */
#define SNAP_OUT 2.5f          /* zooming out of first person comes back to here */
#define SHOULDER 1.75f         /* shift lock: studs to the right */
#define WALL_GAP 0.4f          /* how far from a wall the camera stays */

static f32 ease(f32 dt, f32 rate) { return ub_clampf(dt * rate, 0.0f, 1.0f); }

/* The zoom limits now: the place's, or first person only (LockFirstPerson). */
static void zoom_limits(const ub_game *g, f32 *lo, f32 *hi)
{
    *lo = g->cam.min_zoom > 0 ? g->cam.min_zoom : PLAY_ZOOM_MIN;
    *hi = g->cam.max_zoom >= *lo ? g->cam.max_zoom : PLAY_ZOOM_MAX;
    if (g->cam.camera_mode == 1) *hi = *lo;
}

/* One step of zoom: in (dir < 0) or out, by `factor` (< 1), snapping in
 * and out of first person as Roblox's camera does. */
static f32 zoom_step(f32 z, int dir, f32 factor, f32 lo, f32 hi)
{
    if (dir < 0) {
        z *= factor;
        if (z < SNAP_IN && lo < PLAY_FIRST_PERSON) z = lo;
    } else {
        z = z < PLAY_FIRST_PERSON ? SNAP_OUT : z / factor;
    }
    return ub_clampf(z, lo, hi);
}

void play_view_input(ub_play_view *v, ub_game *g, const ub_input *in, f32 dt, bool keys, bool look, vec3 *move,
                     bool *jump)
{
    const ub_cam_state *cs = &g->cam;
    bool scripted = cs->camera_type == UB_CAMTYPE_SCRIPTABLE || cs->camera_type == UB_CAMTYPE_FIXED;
    f32 lo, hi;
    zoom_limits(g, &lo, &hi);

    /* zoom: the wheel, I and O */
    for (int n = in->wheel; n >= UB_WHEEL_NOTCH; n -= UB_WHEEL_NOTCH) v->zoom = zoom_step(v->zoom, -1, 0.8f, lo, hi);
    for (int n = in->wheel; n <= -UB_WHEEL_NOTCH; n += UB_WHEEL_NOTCH) v->zoom = zoom_step(v->zoom, 1, 0.8f, lo, hi);
    if (keys && in->key_pressed['I']) v->zoom = zoom_step(v->zoom, -1, 0.85f, lo, hi);
    else if (keys && in->key_down['I']) v->zoom = zoom_step(v->zoom, -1, 1.0f - ub_clampf(dt * 1.6f, 0, 0.5f), lo, hi);
    if (keys && in->key_pressed['O']) v->zoom = zoom_step(v->zoom, 1, 0.85f, lo, hi);
    else if (keys && in->key_down['O']) v->zoom = zoom_step(v->zoom, 1, 1.0f - ub_clampf(dt * 1.6f, 0, 0.5f), lo, hi);
    v->zoom = ub_clampf(v->zoom, lo, hi);
    bool was_first = v->first_person;
    v->first_person = v->zoom < PLAY_FIRST_PERSON && !scripted;
    if (v->first_person != was_first) ub_info("camera: %s", v->first_person ? "first person" : "third person");

    /* shift lock: Shift, where the player and the place allow it */
    if (!v->shift_lock_allowed || !cs->mouse_lock_ok) v->shift_lock = false;
    else if (keys && in->key_pressed[UB_KEY_SHIFT]) {
        v->shift_lock = !v->shift_lock;
        ub_info("shift lock %s", v->shift_lock ? "on" : "off");
    }

    /* the mouse: locked to the middle in first person and shift lock (and
     * when a script asks), else turning the camera while the right button
     * is held */
    bool focus = in->has_focus && look && !scripted;
    v->locked = focus && (v->first_person || v->shift_lock || cs->mouse_behavior == UB_MOUSE_LOCK_CENTER);
    bool drag = focus && (in->mouse_down[UB_MOUSE_RIGHT] || cs->mouse_behavior == UB_MOUSE_LOCK_CURRENT);
    bool want_look = v->locked || drag;
    if (want_look != in->mouse_captured) ub_debug("mouse look %s%s", want_look ? "on" : "off", v->locked ? " (locked)" : "");
    plat_set_mouse_capture(want_look);
    if (in->mouse_captured && !scripted) {
        f32 sens = LOOK_SENSITIVITY * (v->look_scale > 0 ? v->look_scale : 1.0f);
        v->yaw += (f32)in->mouse_dx * sens;
        v->pitch -= (f32)in->mouse_dy * sens * (v->invert_y ? -1.0f : 1.0f);
    }
    v->pitch = ub_clampf(v->pitch, PLAY_PITCH_MIN, PLAY_PITCH_MAX);
    if (keys && in->key_down[UB_KEY_LEFT]) v->yaw -= TURN_SPEED * dt;
    if (keys && in->key_down[UB_KEY_RIGHT]) v->yaw += TURN_SPEED * dt;
    v->yaw = ub_wrap_angle(v->yaw);

    /* movement, relative to the camera */
    f32 fx = ub_sinf(v->yaw), fz = ub_cosf(v->yaw);
    f32 rx = fz, rz = -fx;
    f32 mf = 0, mr = 0;
    if (keys) {
        if (in->key_down['W'] || in->key_down[UB_KEY_UP]) mf += 1;
        if (in->key_down['S'] || in->key_down[UB_KEY_DOWN]) mf -= 1;
        if (in->key_down['D']) mr += 1;
        if (in->key_down['A']) mr -= 1;
    }
    if (mf != 0 && mr != 0) { mf *= 0.7071f; mr *= 0.7071f; }
    *move = v3(fx * mf + rx * mr, 0, fz * mf + rz * mr);
    v->moving = mf != 0 || mr != 0;
    if ((v->first_person || v->shift_lock) && !scripted && !g->dead) {
        v->facing = v->yaw; /* it faces where the camera looks, and walks sideways and back */
    } else if (v->moving) {
        /* turn the character smoothly towards where it is going
         * (yaw of direction (x, z) is atan2(x, z): yaw 0 faces +Z) */
        f32 want = ub_atan2f(move->x, move->z);
        f32 diff = ub_wrap_angle(want - v->facing);
        v->facing = ub_wrap_angle(v->facing + diff * ub_clampf(dt * 12.0f, 0, 1));
    }
    /* Roblox-style: holding Space keeps jumping each time the feet land */
    *jump = keys && (in->key_down[UB_KEY_SPACE] || in->key_pressed[UB_KEY_SPACE]);
}

/* Where the camera looks from: the subject's head (a part when a script
 * made one the CameraSubject). */
static vec3 camera_focus(const ub_game *g)
{
    i32 s = g->cam.subject;
    if (s >= 0 && s != g->humanoid && ub_world_alive(&g->world, s) && g->world.inst[s].part >= 0) {
        vec3 p;
        mat3 r;
        ub_game_part_pose(g, g->world.inst[s].part, &p, &r);
        return p;
    }
    return v3_add(g->feet, v3(0, HEAD_ABOVE_FEET, 0));
}

/* How far along a ray the camera can go before a wall (Poppercam): solid
 * parts stop it, see-through ones (half or more) do not. */
static f32 clear_length(const ub_game *g, vec3 from, vec3 dir, f32 len)
{
    if (!g->phys_ok || len <= 0) return len;
    f32 done = 0;
    for (int tries = 0; tries < 6 && done < len; ++tries) {
        ub_phys_hit h;
        if (!ub_phys_raycast(&g->phys, v3_add(from, v3_scale(dir, done)), dir, len - done + WALL_GAP, g->chr.body, &h))
            return len;
        const ub_part *p = h.part >= 0 && h.part < g->world.part_count ? &g->world.parts[h.part] : NULL;
        if (p && p->transparency >= 128) {
            done += h.t + 0.05f; /* look on through it */
            continue;
        }
        f32 t = done + h.t - WALL_GAP;
        return t < 0 ? 0 : t < len ? t : len;
    }
    return len;
}

static ub_camera script_camera(const ub_play_view *v, const ub_game *g)
{
    ub_camera c = v->cam;
    vec3 f = v3_normalize(g->cam.script_fwd);
    c.pos = g->cam.script_pos;
    c.yaw = ub_atan2f(f.x, f.z);
    c.pitch = ub_atan2f(f.y, ub_sqrtf(f.x * f.x + f.z * f.z));
    c.focus = v3_add(c.pos, v3_scale(f, 10));
    return c;
}

void play_view_update(ub_play_view *v, ub_game *g, f32 dt)
{
    if (g->respawned) {
        g->respawned = false;
        v->death_t = 0;
    }
    v->death_t = g->dead ? ub_clampf(v->death_t + dt * 3.0f, 0, 1) : 0;
    if (g->statics_dirty) rebuild_statics(v, g);
    vec3 vel = g->phys_ok ? ub_char_velocity(&g->chr, &g->phys) : v3(0, 0, 0);
    f32 hspeed = ub_sqrtf(vel.x * vel.x + vel.z * vel.z);
    bool walking = v->moving && hspeed > 2.0f && !g->dead;
    v->walk_amount = ub_clampf(v->walk_amount + (walking ? dt : -dt) * 6.0f, 0.0f, 1.0f);
    if (walking) v->walk_phase = ub_wrap_angle(v->walk_phase + dt * 10.0f * hspeed / UB_CHAR_WALK_SPEED);
    /* the game poses the character's rig with these */
    g->pose_from_app = true;
    g->pose_facing = v->facing;
    g->pose_phase = v->walk_phase;
    g->pose_amount = v->walk_amount;
    g->pose_fallen = v->death_t;

    /* the camera */
    f32 lo, hi;
    zoom_limits(g, &lo, &hi);
    v->zoom = ub_clampf(v->zoom, lo, hi); /* (a script may have changed the limits) */
    v->first_person = v->zoom < PLAY_FIRST_PERSON;
    bool scripted = g->cam.camera_type == UB_CAMTYPE_SCRIPTABLE || g->cam.camera_type == UB_CAMTYPE_FIXED;
    if (v->first_person) v->zoom_now = v->zoom; /* straight to the eyes */
    else if (v->zoom_now < PLAY_FIRST_PERSON) v->zoom_now = SNAP_IN;
    else v->zoom_now += (v->zoom - v->zoom_now) * ease(dt, 10.0f);
    v->shoulder += ((v->shift_lock && !v->first_person ? 1.0f : 0.0f) - v->shoulder) * ease(dt, 12.0f);

    vec3 head = camera_focus(g);
    f32 fx = ub_sinf(v->yaw), fz = ub_cosf(v->yaw), cp = ub_cosf(v->pitch);
    vec3 fwd = v3(fx * cp, ub_sinf(v->pitch), fz * cp), right = v3(fz, 0, -fx);
    /* from the head out to the shoulder (shift lock), then back from there */
    f32 side = clear_length(g, head, right, SHOULDER * v->shoulder);
    vec3 focus = v3_add(head, v3_scale(right, side));
    f32 want = v->first_person ? 0.0f : v->zoom_now;
    f32 len = g->cam.occlusion == 0 ? clear_length(g, focus, v3_scale(fwd, -1), want) : want;
    /* walls pull the camera in at once; it eases back out */
    if (len < v->cam_len || v->first_person) v->cam_len = len;
    else v->cam_len += (len - v->cam_len) * ease(dt, 8.0f);

    ub_camera cam;
    memset(&cam, 0, sizeof cam);
    cam.yaw = v->yaw;
    cam.pitch = v->pitch;
    cam.pos = v3_sub(focus, v3_scale(fwd, v->cam_len));
    cam.focus = focus;
    f32 fov = g->script_fov > 1 ? g->script_fov : v->fov_deg > 10 ? v->fov_deg : 70.0f;
    cam.fov_y = ub_clampf(fov, 1, 120) * UB_PI / 180.0f;
    cam.far_dist = 600.0f + (hi > 128 ? hi : 0);
    if (scripted) {
        /* Scriptable: where a script put it; Fixed: where it was */
        if (g->cam.camera_type == UB_CAMTYPE_SCRIPTABLE && g->cam.script_cf) cam = script_camera(v, g);
        else if (v->have_cam) cam = v->cam;
        cam.fov_y = ub_clampf(fov, 1, 120) * UB_PI / 180.0f;
    }
    v->cam = cam;
    v->have_cam = true;

    /* close up, the character fades; in first person it is not drawn (its
     * tools still are) */
    f32 dist = scripted ? 100.0f : v->cam_len;
    v->fade = v->first_person && !scripted ? 1.0f : ub_clampf((2.5f - dist) / 1.5f, 0.0f, 1.0f);

    /* what the camera did, for scripts and the client UI */
    g->cam.first_person = v->first_person && !scripted;
    g->cam.shift_lock = v->shift_lock && !scripted;
    g->cam.mouse_locked = v->locked;
    g->cam.distance = scripted ? v3_length(v3_sub(cam.pos, head)) : v->cam_len;
}

ub_camera play_view_camera(const ub_play_view *v, const ub_game *g)
{
    if (v->have_cam) return v->cam;
    /* (before the first update) */
    ub_camera cam;
    memset(&cam, 0, sizeof cam);
    vec3 target = camera_focus(g);
    f32 fx = ub_sinf(v->yaw), fz = ub_cosf(v->yaw), cp = ub_cosf(v->pitch);
    cam.yaw = v->yaw;
    cam.pitch = v->pitch;
    cam.pos = v3_sub(target, v3_scale(v3(fx * cp, ub_sinf(v->pitch), fz * cp), v->zoom));
    cam.focus = target;
    cam.fov_y = ub_clampf(v->fov_deg > 10 ? v->fov_deg : 70.0f, 1, 120) * UB_PI / 180.0f;
    cam.far_dist = 600.0f;
    return cam;
}

void play_view_draw(ub_play_view *v, ub_game *g, bool character)
{
    /* the static set was handed over; live parts here: moving ones blended
     * between the last two physics steps, and any a script changed */
    for (i32 i = 0; i < g->live_count; ++i) {
        i32 pi = g->live[i];
        if (!ub_game_part_visible(g, pi)) continue;
        ub_part_draw d = play_part_draw(&g->world, &g->world.parts[pi]);
        if (v->fade > 0.01f && body_part(g, ub_world_part_inst(&g->world, pi))) {
            /* close up the character fades out (Roblox's LocalTransparencyModifier) */
            if (v->fade > 0.97f) continue;
            u32 t = g->world.parts[pi].transparency, f = (u32)(v->fade * 255.0f);
            d.flags = (u8)((d.flags & UB_DRAW_GLOW) | ub_draw_see_through((u8)(t > f ? t : f)));
        }
        ub_game_part_pose(g, pi, &d.pos, &d.rot);
        ub_render_part(&d);
    }
    /* visible ropes, rods and springs: thin cylinders between their ends */
    vec3 la[64], lb[64];
    u32 lc[64];
    int nl = ub_constraints_lines(g, la, lb, lc, 64);
    for (int i = 0; i < nl; ++i) {
        vec3 d = v3_sub(lb[i], la[i]);
        f32 len = v3_length(d);
        if (len < 0.01f) continue;
        vec3 y = v3_scale(d, 1.0f / len);
        vec3 x = v3_normalize(v3_cross(ub_absf(y.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0), y));
        ub_part_draw r;
        memset(&r, 0, sizeof r);
        r.pos = v3_add(la[i], v3_scale(d, 0.5f));
        r.rot.x = x;
        r.rot.y = y;
        r.rot.z = v3_cross(x, y);
        r.size = v3(0.15f, len, 0.15f);
        r.color = lc[i];
        r.shape = UB_SHAPE_CYLINDER;
        r.material = UB_MAT_PLASTIC;
        ub_render_part(&r);
    }
    if (character && !v->first_person) play_draw_blob_shadow(g); /* (the character itself is its rig's parts, drawn above) */
}

/* A soft round shadow on the ground under the character, for when the
 * renderer casts no shadows (software, or OpenGL on low): a flat dark
 * disc drawn see-through, smaller and fainter the higher it jumps. */
void play_draw_blob_shadow(ub_game *g)
{
    if (!g->phys_ok || g->dead) return;
    if (ub_render_backend() == UB_BACKEND_OPENGL && ub_render_quality() != UB_QUALITY_LOW) return;
    ub_phys_hit h;
    vec3 from = v3_add(g->feet, v3(0, 0.5f, 0));
    if (!ub_phys_raycast(&g->phys, from, v3(0, -1, 0), 40.0f, g->chr.body, &h) || h.normal.y < 0.6f) return;
    f32 height = h.t - 0.5f;
    if (height < 0) height = 0;
    f32 k = 1.0f - height / 40.0f; /* 1 on the ground, 0 at 40 studs */
    ub_part_draw d;
    memset(&d, 0, sizeof d);
    d.pos = v3_add(h.point, v3(0, 0.04f, 0));
    d.rot = mat3_identity();
    d.size = v3(3.2f * (0.4f + 0.6f * k), 0.05f, 3.2f * (0.4f + 0.6f * k));
    d.color = 0x141414;
    d.shape = UB_SHAPE_CYLINDER;
    d.material = UB_MAT_PLASTIC;
    d.flags = ub_draw_see_through((u8)(255 - (int)(150.0f * k))); /* about half covered on the ground */
    ub_render_part(&d);
}

/* ------------------------------------------------------------------ GUI */

void play_gui_screen_size(const ub_frame *frame, int *w, int *h)
{
    if (frame->backend != UB_BACKEND_OPENGL && frame->fb) {
        *w = frame->fb->width;
        *h = frame->fb->height;
    } else {
        *w = frame->width;
        *h = frame->height;
    }
}

void play_gui_input(ub_game *g, const ub_frame *frame, const ub_input *in, const ub_camera *cam, bool look, f64 time)
{
    if (!g->gui) return;
    int sw, sh;
    play_gui_screen_size(frame, &sw, &sh);
    ub_gui_input gi;
    memset(&gi, 0, sizeof gi);
    /* window pixels -> GUI screen pixels (smaller when software renders below the window size) */
    gi.mouse_x = frame->width > 0 ? (f32)in->mouse_x * (f32)sw / (f32)frame->width : 0;
    gi.mouse_y = frame->height > 0 ? (f32)in->mouse_y * (f32)sh / (f32)frame->height : 0;
    if (g->cam.mouse_locked) {
        /* first person and shift lock: the mouse is the middle of the screen
         * (clicks, tools and ClickDetectors aim there) */
        gi.mouse_x = (f32)sw * 0.5f;
        gi.mouse_y = (f32)sh * 0.5f;
    }
    for (int b = 0; b < 3; ++b) {
        gi.down[b] = in->mouse_down[b];
        gi.pressed[b] = in->mouse_pressed[b];
        gi.released[b] = in->mouse_released[b];
    }
    gi.wheel = in->wheel / UB_WHEEL_NOTCH;
    gi.text = in->text;
    memcpy(gi.key_pressed, in->key_repeat, sizeof gi.key_pressed);
    gi.mouse_free = (!look || g->cam.mouse_locked) && in->has_focus && (!in->mouse_captured || g->cam.mouse_locked);
    ub_gui_update(g->gui, &g->world, sw, sh, cam, &gi, time);
    /* and the same input for scripts */
    ub_game_input *si = &g->input;
    si->mouse_dx = (f32)in->mouse_dx;
    si->mouse_dy = (f32)in->mouse_dy;
    si->mouse_x = gi.mouse_x;
    si->mouse_y = gi.mouse_y;
    memcpy(si->down, gi.down, sizeof si->down);
    memcpy(si->pressed, gi.pressed, sizeof si->pressed);
    memcpy(si->released, gi.released, sizeof si->released);
    si->wheel = gi.wheel;
    memcpy(si->key_down, in->key_down, sizeof si->key_down);
    memcpy(si->key_pressed, in->key_pressed, sizeof si->key_pressed);
    memcpy(si->key_released, in->key_released, sizeof si->key_released);
    si->gui_mouse = ub_gui_mouse_taken(g->gui);
    si->gui_keys = ub_gui_keyboard_taken(g->gui);
    si->focused = in->has_focus;
    si->screen_w = sw;
    si->screen_h = sh;
    if (cam) {
        si->cam = *cam;
        si->have_cam = true;
    }
}

void play_gui_draw_world(ub_game *g, const ub_camera *cam)
{
    if (!g->gui) return;
    ub_gui_render(g->gui, &g->world, cam);
    const ub_gui_quad *q;
    int n = ub_gui_quads(g->gui, &q);
    for (int i = 0; i < n; ++i) ub_render_image_quad(&q[i].image, q[i].corners, q[i].on_top, q[i].brightness);
}

void play_gui_draw_screen(ub_game *g)
{
    if (!g->gui) return;
    const int *rects;
    int n;
    const ub_render_image *img = ub_gui_screen(g->gui, &rects, &n);
    if (img) ub_render_overlay(img, rects, n);
}
