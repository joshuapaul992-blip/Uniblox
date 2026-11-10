/* Uniblox - playing a place on screen (see app_play.h). */
#include "app/app_play.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "crt/ub_crt.h"

#define TURN_SPEED 1.8f        /* radians/s for arrow keys */
#define LOOK_SENSITIVITY 0.006f

/* A part as the renderer wants it. */
ub_part_draw play_part_draw(const ub_part *p)
{
    ub_part_draw d;
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
    v->zoom = 22.0f;
    v->look_scale = 1.0f;
    v->fov_deg = 70.0f;
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
        if (ub_game_part_visible(g, i) && !ub_game_part_live(g, i)) v->statics[v->static_count++] = play_part_draw(&w->parts[i]);
    ub_render_set_static(v->statics, v->static_count);
    g->statics_dirty = false;
}

void play_view_reset(ub_play_view *v, ub_game *g)
{
    v->death_t = 0;
    v->walk_amount = 0;
    rebuild_statics(v, g);
}

void play_view_input(ub_play_view *v, const ub_input *in, f32 dt, bool keys, bool look, vec3 *move, bool *jump)
{
    bool want_look = in->mouse_down[UB_MOUSE_RIGHT] && in->has_focus && look;
    if (want_look != in->mouse_captured) ub_debug("mouse look %s", want_look ? "on" : "off");
    plat_set_mouse_capture(want_look);
    f32 sens = LOOK_SENSITIVITY * (v->look_scale > 0 ? v->look_scale : 1.0f);
    v->yaw += (f32)in->mouse_dx * sens;
    v->pitch = ub_clampf(v->pitch - (f32)in->mouse_dy * sens * (v->invert_y ? -1.0f : 1.0f), PLAY_PITCH_MIN, PLAY_PITCH_MAX);
    if (keys && in->key_down[UB_KEY_LEFT]) v->yaw -= TURN_SPEED * dt;
    if (keys && in->key_down[UB_KEY_RIGHT]) v->yaw += TURN_SPEED * dt;
    v->yaw = ub_wrap_angle(v->yaw);
    for (int n = in->wheel; n >= UB_WHEEL_NOTCH; n -= UB_WHEEL_NOTCH) v->zoom *= 0.88f;
    for (int n = in->wheel; n <= -UB_WHEEL_NOTCH; n += UB_WHEEL_NOTCH) v->zoom /= 0.88f;
    v->zoom = ub_clampf(v->zoom, PLAY_ZOOM_MIN, PLAY_ZOOM_MAX);

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
    if (v->moving) {
        /* turn the character smoothly towards where it is going
         * (yaw of direction (x, z) is atan2(x, z): yaw 0 faces +Z) */
        f32 want = ub_atan2f(move->x, move->z);
        f32 diff = ub_wrap_angle(want - v->facing);
        v->facing = ub_wrap_angle(v->facing + diff * ub_clampf(dt * 12.0f, 0, 1));
    }
    /* Roblox-style: holding Space keeps jumping each time the feet land */
    *jump = keys && (in->key_down[UB_KEY_SPACE] || in->key_pressed[UB_KEY_SPACE]);
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
}

ub_camera play_view_camera(const ub_play_view *v, const ub_game *g)
{
    ub_camera cam;
    vec3 target = v3_add(g->feet, v3(0, 3.5f, 0));
    f32 fx = ub_sinf(v->yaw), fz = ub_cosf(v->yaw), cp = ub_cosf(v->pitch);
    cam.yaw = v->yaw;
    cam.pitch = v->pitch;
    vec3 fwd = v3(fx * cp, ub_sinf(v->pitch), fz * cp);
    cam.pos = v3_sub(target, v3_scale(fwd, v->zoom));
    cam.focus = target;
    cam.fov_y = (v->fov_deg > 10 ? v->fov_deg : 70.0f) * UB_PI / 180.0f;
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
        ub_part_draw d = play_part_draw(&g->world.parts[pi]);
        ub_game_part_pose(g, pi, &d.pos, &d.rot);
        ub_render_part(&d);
    }
    if (character) {
        play_draw_character(g->feet.x, g->feet.y, g->feet.z, v->facing, v->walk_phase, v->walk_amount, v->death_t);
        play_draw_blob_shadow(g);
    }
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
    d.pos = v3_add(h.point, v3(0, 0.04f, 0));
    d.rot = mat3_identity();
    d.size = v3(3.2f * (0.4f + 0.6f * k), 0.05f, 3.2f * (0.4f + 0.6f * k));
    d.color = 0x141414;
    d.shape = UB_SHAPE_CYLINDER;
    d.material = UB_MAT_PLASTIC;
    d.flags = ub_draw_see_through((u8)(255 - (int)(150.0f * k))); /* about half covered on the ground */
    ub_render_part(&d);
}
