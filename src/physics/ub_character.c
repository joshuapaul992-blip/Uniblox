#include "physics/ub_character.h"
#include "core/ub_math.h"

#define GROUND_ACCEL 220.0f     /* studs/s^2: reaches walking speed in ~0.07 s */
#define AIR_ACCEL 110.0f
#define SNAP_DOWN 0.6f          /* stays on the ground walking down slopes and small steps */
#define FOOT_SPREAD 0.6f        /* the four outer ground rays, from the centre */
#define MIN_GROUND_NORMAL 0.45f /* surfaces steeper than ~63 degrees are walls */

static const vec3 k_spheres[2] = { { 0, UB_CHAR_STEP_UP + UB_CHAR_RADIUS, 0 },
                                   { 0, UB_CHAR_HEIGHT - UB_CHAR_RADIUS, 0 } };

bool ub_char_init(ub_character *c, ub_physics *ph, vec3 feet)
{
    c->body = ub_phys_add_character(ph, feet, k_spheres, 2, UB_CHAR_RADIUS, UB_CHAR_MASS);
    c->grounded = false;
    c->ground_body = -1;
    c->ground_vel = v3(0, 0, 0);
    c->ground_point = feet;
    c->ground_part = -1;
    c->walk_speed = UB_CHAR_WALK_SPEED;
    ub_char_set_jump_height(c, ph, UB_CHAR_JUMP_HEIGHT);
    c->jumped = false;
    c->landed_speed = 0;
    return c->body >= 0;
}

void ub_char_set_jump_height(ub_character *c, const ub_physics *ph, f32 height)
{
    f32 g = -ph->gravity.y;
    c->jump_speed = ub_sqrtf(2.0f * (g > 1.0f ? g : 1.0f) * (height > 0 ? height : 0));
}

void ub_char_teleport(ub_character *c, ub_physics *ph, vec3 feet)
{
    ub_phys_teleport(ph, c->body, feet);
    c->grounded = false;
    c->ground_body = -1;
    c->ground_part = -1;
    c->ground_vel = v3(0, 0, 0);
}

void ub_char_before_step(ub_character *c, ub_physics *ph, vec3 move, bool jump)
{
    ub_body *b = &ph->bodies[c->body];
    const f32 dt = UB_PHYS_DT;
    /* walking: steer the horizontal velocity towards the wanted one, with
     * limited force, so heavy things cannot simply be shoved aside */
    vec3 want = v3(move.x * c->walk_speed, 0, move.z * c->walk_speed);
    if (c->grounded) want = v3_add(want, v3(c->ground_vel.x, 0, c->ground_vel.z));
    vec3 dv = v3(want.x - b->v.x, 0, want.z - b->v.z);
    f32 most = (c->grounded ? GROUND_ACCEL : AIR_ACCEL) * dt, len = v3_length(dv);
    if (len > most) dv = v3_scale(dv, most / len);
    b->v.x += dv.x;
    b->v.z += dv.z;
    if (jump && c->grounded) {
        /* half a step of gravity: with this integration the jump then peaks at the height asked */
        b->v.y = c->jump_speed + UB_MAX(c->ground_vel.y, 0.0f) + ph->gravity.y * dt * 0.5f;
        c->grounded = false;
        c->jumped = true;
    } else if (c->grounded) {
        b->v.y = c->ground_vel.y;
    } else {
        b->v = v3_madd(b->v, ph->gravity, dt); /* the engine leaves gravity to us */
    }
}

void ub_char_after_step(ub_character *c, ub_physics *ph)
{
    ub_body *b = &ph->bodies[c->body];
    vec3 feet = b->pos;
    /* legs: rays down from a step's height above the feet */
    f32 reach = UB_CHAR_STEP_UP + (c->grounded ? SNAP_DOWN : UB_MAX(0.0f, -b->v.y * UB_PHYS_DT) + 0.05f);
    static const f32 offs[5][2] = { { 0, 0 }, { FOOT_SPREAD, 0 }, { -FOOT_SPREAD, 0 }, { 0, FOOT_SPREAD }, { 0, -FOOT_SPREAD } };
    bool found = false;
    ub_phys_hit best;
    for (int i = 0; i < 5; ++i) {
        vec3 o = v3(feet.x + offs[i][0], feet.y + UB_CHAR_STEP_UP, feet.z + offs[i][1]);
        ub_phys_hit h;
        if (!ub_phys_raycast(ph, o, v3(0, -1, 0), reach, c->body, &h)) continue;
        if (h.t < 1e-4f || h.normal.y < MIN_GROUND_NORMAL) continue; /* started inside something, or a wall */
        if (!found || h.point.y > best.point.y) best = h;
        found = true;
    }
    bool rising = b->v.y > (c->grounded ? c->ground_vel.y : 0.0f) + 0.5f;
    if (found && !rising && (feet.y <= best.point.y + 0.02f || c->grounded)) {
        if (!c->grounded) c->landed_speed = UB_MAX(c->landed_speed, -b->v.y);
        c->grounded = true;
        c->ground_body = best.body;
        c->ground_part = best.part;
        c->ground_point = best.point;
        c->ground_vel = ub_phys_point_velocity(ph, best.body, best.point);
        b->v.y = c->ground_vel.y;
        ub_phys_set_position(ph, c->body, v3(feet.x, best.point.y, feet.z));
        /* our weight presses on loose things we stand on (sleepers stay asleep) */
        const ub_body *g = &ph->bodies[best.body];
        if (g->type == UB_BODY_DYNAMIC && g->awake)
            ub_phys_apply_impulse(ph, best.body, v3_scale(ph->gravity, UB_CHAR_MASS * UB_PHYS_DT), best.point);
    } else {
        c->grounded = false;
        c->ground_body = -1;
        c->ground_part = -1;
    }
}

vec3 ub_char_feet(const ub_character *c, const ub_physics *ph, f32 alpha)
{
    vec3 p;
    mat3 r;
    ub_phys_body_pose(ph, c->body, alpha, &p, &r);
    return p;
}

vec3 ub_char_velocity(const ub_character *c, const ub_physics *ph)
{
    return ph->bodies[c->body].v;
}
