/* Uniblox - rigid body physics.
 *
 * Every assembly of welded parts under the Workspace becomes one body.
 * Assemblies with an anchored part never move (they are "static" and live
 * in a bounding-volume tree built once). Unanchored assemblies fall,
 * tumble, stack and come to rest; resting groups go to sleep and wake when
 * something touches them. Kinematic bodies are moved by the game (like
 * the demo's spinning block) and push things. The character is a special
 * body that the character controller steers.
 *
 * Collisions are found 120 times a second; each step is solved in six
 * substeps (720 a second) with soft contacts, the "soft step" scheme of
 * Box2D 3, which keeps stacks about 20 parts tall steady under Roblox's
 * strong gravity (196.2 studs/s^2). Contacts are kept from step to step and their impulses
 * reused (warm starting); friction and bounce come from the material.
 * Loose parts that fall below the world's fallen-parts height are removed.
 *
 * The world's parts are updated after each step, so the renderer and
 * place saving see where things are. */
#ifndef UB_PHYSICS_H
#define UB_PHYSICS_H

#include "physics/ub_collide.h"
#include "physics/ub_pmath.h"
#include "world/ub_world.h"

#define UB_PHYS_HZ 120
#define UB_PHYS_DT (1.0f / (f32)UB_PHYS_HZ)

typedef enum {
    UB_BODY_STATIC,       /* body 0 holds every anchored part */
    UB_BODY_DYNAMIC,
    UB_BODY_KINEMATIC,    /* moved by the game, infinite mass */
    UB_BODY_CHARACTER,    /* moved by the character controller; never rotates */
} ub_body_type;

typedef struct {
    vec3 pos;             /* centre of mass */
    quat q;
    mat3 rot;             /* from q */
    vec3 v, w;            /* linear and angular velocity (world) */
    f32 mass, inv_mass;
    mat3 inv_inertia_local, inv_inertia;  /* body space; world space */
    vec3 prev_pos;        /* pose before the last step, for smooth drawing */
    quat prev_q;
    vec3 target_pos;      /* kinematic: where the game wants it next step */
    quat target_q;
    i32 first_shape, shape_count;
    u8 type;              /* ub_body_type */
    u8 awake, fallen, has_target;
    f32 sleep_time;
    i32 sleep_next, sleep_head;   /* the group it went to sleep with */
    u32 woke_step;        /* step it last woke: contacts that come back then are not new touches */
} ub_body;

enum {
    UB_SHAPE_COLLIDES = 1,  /* solid */
    UB_SHAPE_SENSOR = 2,    /* not solid, but reports touches (CanCollide off, watched by a script) */
};

typedef struct {
    i32 body;
    i32 part;             /* world part index, -1 for none (the character) */
    u8 shape;             /* ub_shape (UB_SHAPE_BALL = sphere) */
    u8 flags;             /* UB_SHAPE_* */
    u8 in_dyn;            /* listed in the moving shapes */
    vec3 size;
    f32 radius;           /* spheres */
    f32 bound;            /* radius of a sphere around the shape */
    vec3 lpos;            /* in body space */
    mat3 lrot;
    vec3 wpos;            /* in the world, after the last step */
    mat3 wrot;
    vec3 lo, hi;          /* world bounding box */
    f32 friction, restitution;
    const ub_hull *hull;  /* a piece of the part's own mesh (unit cube), or NULL: the shape's */
    i32 next_piece;       /* the part's next piece (anchored unions and MeshParts), -1 none */
} ub_pshape;

typedef struct ub_manifold ub_manifold;

/* Joints between bodies (milestone 8C), for Roblox's constraints. Anchors
 * and axes are in each body's own space; body 0 is the static world. */
typedef enum {
    UB_PJ_BALL,           /* the anchors meet */
    UB_PJ_HINGE,          /* the anchors meet; the axes stay together; it turns about them */
    UB_PJ_PRISMATIC,      /* slides along the axis, never turns */
    UB_PJ_ROPE,           /* the anchors at most `length` apart */
    UB_PJ_ROD,            /* exactly `length` apart */
    UB_PJ_SPRING,         /* pulled towards `length` by stiffness and damping (a force, not a constraint) */
} ub_pjoint_kind;

typedef struct {
    u8 kind;              /* ub_pjoint_kind */
    u8 limits;            /* hinge: lower..upper angle; prismatic: lower..upper distance */
    u8 motor;             /* 0 none, 1 motor (motor_speed), 2 servo (towards target, at motor_speed) */
    i32 a, b;             /* bodies */
    vec3 la, lb;          /* anchors */
    vec3 axa, axb;        /* hinge / slide axes (unit) */
    vec3 refa, refb;      /* hinge: perpendicular reference axes, for its angle */
    f32 length, stiffness, damping;
    f32 lower, upper;
    f32 motor_speed, motor_max, target;   /* rad/s or studs/s; max torque or force; servo target */
    f32 angle;            /* hinge: its angle now (read back), prismatic: its position */
} ub_pjoint;

/* Two parts that started touching (world part indices; -1 = no part, i.e.
 * the character). */
typedef struct {
    i32 a, b;
} ub_touch;
typedef struct ub_bvh_node ub_bvh_node;

typedef struct {
    i32 bodies, awake, kinematic, fallen;
    i32 static_shapes, moving_shapes;
    i32 pairs, manifolds, contacts;
    i32 welds;
    u32 steps;
} ub_phys_stats;

typedef struct {
    ub_world *world;
    vec3 gravity;
    vec3 up;              /* against gravity */
    f32 fallen_height;

    ub_body *bodies;
    i32 body_count, body_cap;
    i32 *free_body;       /* emptied one-shape bodies, reused for new parts */
    i32 free_body_count, free_body_ready, free_body_cap; /* [0, ready) usable; the rest after the next step */
    ub_pshape *shapes;
    i32 shape_count, shape_cap;
    i32 *part_shape;      /* world part -> shape, -1 if not simulated */
    i32 part_count;

    /* static bounding-volume tree */
    ub_bvh_node *nodes;
    i32 node_count;
    i32 *static_ids;      /* static shape indices, in tree leaf order */
    i32 static_count;

    /* moving shapes, kept sorted by the low x of their boxes */
    i32 *dyn;
    i32 dyn_count, dyn_cap;

    u64 *pairs, *pairs_tmp;
    i32 pair_count, pair_cap, pairs_tmp_cap;
    ub_manifold *man, *man_prev;   /* this step's contacts, and last step's */
    i32 man_count, man_cap, man_prev_count, man_prev_cap;

    i32 *scratch;         /* per-body scratch (islands) */
    f32 *scratch_f;
    i32 scratch_cap, scratch_f_cap;

    /* touches that began in the last step */
    ub_touch *touches;
    /* joints (8C) and the forces to apply in the next step */
    ub_pjoint *joints;
    i32 joint_count, joint_cap;
    vec3 *force, *torque;     /* per body, cleared after each step */
    vec3 *drive_v, *drive_f, *drive_w, *drive_t; /* per body: velocity targets and their limits */
    u8 *driven;
    i32 force_cap;
    i32 touch_count, touch_cap;
    u64 *sensor_keys, *sensor_prev;   /* sensor pairs touching (sorted) */
    i32 sensor_count, sensor_cap, sensor_prev_count, sensor_prev_cap;

    ub_phys_stats stats;
    bool all_asleep_logged;
} ub_physics;

typedef struct {
    f32 t;
    vec3 point, normal;
    i32 body, shape, part;
} ub_phys_hit;

/* Builds bodies from the world's parts. Parts listed in `kinematic_parts`
 * (with the parts welded to them) become kinematic. Returns false if out
 * of memory. The world must outlive the physics. */
bool ub_phys_init(ub_physics *ph, ub_world *w, const i32 *kinematic_parts, int kinematic_count);
void ub_phys_free(ub_physics *ph);

/* One fixed step of UB_PHYS_DT. */
void ub_phys_step(ub_physics *ph);

/* A character body: spheres at `offsets` from `pos`. Returns its index. */
i32 ub_phys_add_character(ub_physics *ph, vec3 pos, const vec3 *offsets, int count, f32 radius, f32 mass);
void ub_phys_teleport(ub_physics *ph, i32 body, vec3 pos);
/* Moves a body without touching its velocity (and so keeps smooth drawing). */
void ub_phys_set_position(ub_physics *ph, i32 body, vec3 pos);

/* Kinematic bodies: where to be at the end of the next step. */
void ub_phys_set_target(ub_physics *ph, i32 body, vec3 pos, const mat3 *rot);

void ub_phys_wake(ub_physics *ph, i32 body);
void ub_phys_apply_impulse(ub_physics *ph, i32 body, vec3 impulse, vec3 point);
vec3 ub_phys_point_velocity(const ub_physics *ph, i32 body, vec3 point);

/* Nearest collidable shape along the ray (dir normalised), skipping one
 * body (-1 = none). */
bool ub_phys_raycast(const ub_physics *ph, vec3 origin, vec3 dir, f32 max_dist, i32 ignore_body, ub_phys_hit *hit);

/* After a script changes a part (or adds one): brings the simulation in
 * line with it. `alive`: in the Workspace and not destroyed. `sensor`:
 * report its touches even if it is not solid. Moving an anchored part makes
 * it kinematic, so it pushes what it moves into. */
void ub_phys_sync_part(ub_physics *ph, i32 part, bool alive, bool sensor);
void ub_phys_set_gravity(ub_physics *ph, f32 gravity);

/* The body a part belongs to (0 = static), or -1 if it is not simulated. */
i32 ub_phys_part_body(const ub_physics *ph, i32 part);
/* Where a moving part is, blended between the last two steps by alpha
 * (0..1) for smooth drawing. False if the part is not moving or has
 * fallen out of the world. */
bool ub_phys_part_pose(const ub_physics *ph, i32 part, f32 alpha, vec3 *pos, mat3 *rot);
/* The same for a body's centre. */
void ub_phys_body_pose(const ub_physics *ph, i32 body, f32 alpha, vec3 *pos, mat3 *rot);

void ub_phys_get_stats(const ub_physics *ph, ub_phys_stats *out);

/* Joints (8C): replaces all of them (bodies they join wake up). */
bool ub_phys_set_joints(ub_physics *ph, const ub_pjoint *j, i32 count);
/* A force and torque (world, through the centre of mass) on a body for
 * the next step: body movers, VectorForce, Torque. */
void ub_phys_add_force(ub_physics *ph, i32 body, vec3 force, vec3 torque);
/* Moves a body's velocities towards targets, limited to what max force
 * and max torque could do in one step (LinearVelocity, BodyVelocity,
 * AngularVelocity, AlignPosition, BodyGyro...). A component is left
 * alone where its max is 0. */
void ub_phys_drive(ub_physics *ph, i32 body, vec3 v_target, vec3 max_force, vec3 w_target, vec3 max_torque);

/* Material properties: density (mass per cubic stud), friction, bounce. */
void ub_material_physics(u8 material, f32 *density, f32 *friction, f32 *restitution);

#endif
