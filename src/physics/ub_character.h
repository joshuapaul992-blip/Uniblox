/* Uniblox - the player character's movement, in the style of a Roblox
 * humanoid.
 *
 * The body is two spheres stacked above the legs; they collide with walls,
 * ceilings and loose parts (and push the loose ones). The legs are rays:
 * the character stands on whatever is under its feet, steps up anything up
 * to STEP_UP studs high without jumping, and follows moving platforms. Its
 * weight presses on loose parts it stands on.
 *
 * Call ub_char_before_step and ub_char_after_step around every physics
 * step. Speeds are Roblox's defaults: walking 16 studs/s, jumping 7.2 studs
 * high. */
#ifndef UB_CHARACTER_H
#define UB_CHARACTER_H

#include "physics/ub_physics.h"

#define UB_CHAR_STEP_UP 2.2f       /* highest ledge walked up without jumping */
#define UB_CHAR_RADIUS 1.0f        /* body width: 2 studs */
#define UB_CHAR_HEIGHT 5.4f        /* feet to the top of the head */
#define UB_CHAR_WALK_SPEED 16.0f
#define UB_CHAR_JUMP_HEIGHT 7.2f
#define UB_CHAR_MASS 12.0f

typedef struct {
    i32 body;
    bool grounded;
    i32 ground_body;          /* what it stands on (-1 = nothing) */
    vec3 ground_point, ground_vel;
    i32 ground_part;          /* world part it stands on, -1 = none */
    f32 walk_speed;           /* studs/s (Humanoid.WalkSpeed) */
    f32 jump_speed;           /* from the jump height and gravity */
    /* events since the last ub_char_take_events */
    bool jumped;
    f32 landed_speed;         /* > 0: hit the ground this fast */
} ub_character;

/* Adds the character's body to the physics, feet at `feet`. */
bool ub_char_init(ub_character *c, ub_physics *ph, vec3 feet);
void ub_char_teleport(ub_character *c, ub_physics *ph, vec3 feet);
void ub_char_set_jump_height(ub_character *c, const ub_physics *ph, f32 height);

/* move: wanted direction on the ground (x, z; length up to 1). */
void ub_char_before_step(ub_character *c, ub_physics *ph, vec3 move, bool jump);
void ub_char_after_step(ub_character *c, ub_physics *ph);

/* Where the feet are; alpha blends between the last two steps. */
vec3 ub_char_feet(const ub_character *c, const ub_physics *ph, f32 alpha);
vec3 ub_char_velocity(const ub_character *c, const ub_physics *ph);

#endif
