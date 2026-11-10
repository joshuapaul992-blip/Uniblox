/* Uniblox - the editor's model, and the milestone 6 exit test: an obstacle
 * course built with editor actions only (no code written), then played by
 * the character. */
#include <math.h>
#include <stdlib.h>

#include "editor/ub_editor.h"
#include "game/ub_game.h"
#include "render/ub_mesh.h"
#include "render/ub_texture.h"
#include "script/ub_script.h"
#include "test.h"

static ub_editor g_ed;

static bool vnear(vec3 a, vec3 b, f32 eps) { return fabsf(a.x - b.x) < eps && fabsf(a.y - b.y) < eps && fabsf(a.z - b.z) < eps; }

static const ub_part *part_of(const ub_editor *e, i32 inst) { return &e->world.parts[e->world.inst[inst].part]; }

static void test_basics(void)
{
    ub_editor *e = &g_ed;
    ed_init(e);
    i32 ws = e->world.workspace;
    CHECK(ub_world_find_child(&e->world, ws, "Baseplate") >= 0 && ub_world_find_child(&e->world, ws, "SpawnLocation") >= 0);
    CHECK(!e->dirty && !ed_undo_name(e));

    /* insert: snapped to the grid, sitting on the point, selected */
    i32 a = ed_insert(e, ED_INS_BLOCK, v3(3.4f, 0, -2.6f));
    CHECK(a >= 0 && ed_selected(e, a) && e->sel_count == 1 && e->dirty);
    CHECK(vnear(part_of(e, a)->pos, v3(3, 0.5f, -3), 1e-5f) && vnear(part_of(e, a)->size, v3(4, 1, 2), 1e-5f));
    CHECK(strcmp(ed_undo_name(e), "Insert") == 0);

    /* properties in Roblox's terms round-trip (z is flipped) */
    vec3 pos, ori, size;
    ed_get_rbx(e, a, &pos, &ori, &size);
    CHECK(vnear(pos, v3(3, 0.5f, 3), 1e-5f) && vnear(ori, v3(0, 0, 0), 1e-3f) && vnear(size, v3(4, 1, 2), 1e-5f));
    ed_set_rbx(e, a, v3(10, 5, 20), v3(0, 90, 0), v3(6, 2, 3));
    ed_get_rbx(e, a, &pos, &ori, &size);
    CHECK(vnear(pos, v3(10, 5, 20), 1e-4f) && vnear(ori, v3(0, 90, 0), 0.01f) && vnear(size, v3(6, 2, 3), 1e-4f));
    CHECK(vnear(part_of(e, a)->pos, v3(10, 5, -20), 1e-4f));

    /* undo puts it back, selection too; redo again */
    CHECK(ed_undo(e));
    CHECK(vnear(part_of(e, ed_first(e))->pos, v3(3, 0.5f, -3), 1e-4f));
    CHECK(ed_redo(e));
    a = ed_first(e);
    CHECK(a >= 0 && vnear(part_of(e, a)->pos, v3(10, 5, -20), 1e-4f));
    CHECK(ed_undo(e) && ed_undo(e)); /* and the insert */
    CHECK(e->sel_count == 0 && ub_world_find_child(&e->world, e->world.workspace, "Part") < 0);
    CHECK(!ed_undo(e) && ed_redo(e) && ed_redo(e) && !ed_redo(e));
    a = ed_first(e);

    /* repeated changes of the same thing within a second are one step */
    int steps = e->undo_n;
    e->now = 10;
    for (int i = 0; i < 20; ++i) {
        e->now += 0.05f;
        ed_set_transparency(e, (f32)i / 20.0f);
    }
    CHECK(e->undo_n == steps + 1);
    e->now += 5;
    ed_set_transparency(e, 0);
    CHECK(e->undo_n == steps + 2);

    /* colour, material, flags apply to the selection */
    ed_set_color(e, 0x112233);
    ed_set_material(e, UB_MAT_WOOD);
    ed_set_anchored(e, false);
    ed_set_collide(e, false);
    ed_set_glow(e, true);
    const ub_part *p = part_of(e, a);
    CHECK(p->color == 0x112233 && p->material == UB_MAT_WOOD && !(p->flags & UB_PART_ANCHORED) &&
          !(p->flags & UB_PART_CAN_COLLIDE) && (p->flags & UB_PART_GLOW));
    /* shape change keeps the Roblox size: a 6x2x3 block becomes a ball of 2 */
    ed_set_shape(e, UB_SHAPE_BALL);
    CHECK(part_of(e, a)->shape == UB_SHAPE_BALL && vnear(part_of(e, a)->size, v3(2, 2, 2), 1e-4f));
    ed_undo(e);

    /* resize: the opposite face stays put */
    ed_set_rbx(e, a, v3(0, 5, 0), v3(0, 0, 0), v3(4, 1, 2));
    ed_resize(e, a, 0, 2); /* +x face out by 2 */
    CHECK(vnear(part_of(e, a)->size, v3(6, 1, 2), 1e-5f) && vnear(part_of(e, a)->pos, v3(1, 5, 0), 1e-5f));
    ed_resize(e, a, 3, -10); /* -x face: shrinks no smaller than 0.2 */
    CHECK(fabsf(part_of(e, a)->size.x - 0.2f) < 1e-5f && fabsf(part_of(e, a)->pos.x - 3.9f) < 1e-4f);

    /* rotate a quarter turn: exact */
    ed_set_rbx(e, a, v3(0, 5, 0), v3(0, 0, 0), v3(4, 1, 2));
    ed_rotate(e, 1, 90);
    const ub_part *rp = part_of(e, a);
    CHECK(rp->rot.x.x == 0 && fabsf(rp->rot.x.z) == 1 && rp->rot.y.y == 1);

    /* duplicate: beside the original, selected */
    ed_duplicate(e);
    i32 b = ed_first(e);
    CHECK(b >= 0 && b != a && e->sel_count == 1);
    CHECK(fabsf(part_of(e, b)->pos.x - (part_of(e, a)->pos.x + 2)) < 1e-4f); /* turned: 2 wide */

    /* copy, paste (twice), cut */
    ed_copy(e);
    ed_paste(e);
    i32 c = ed_first(e);
    CHECK(c >= 0 && c != b && fabsf(part_of(e, c)->pos.x - (part_of(e, b)->pos.x + 2)) < 1e-4f);
    ed_select(e, a, false);
    ed_cut(e);
    CHECK(!ub_world_alive(&e->world, a) && strcmp(ed_undo_name(e), "Cut") == 0);
    ed_paste(e);
    CHECK(e->sel_count == 1);
    ed_translate(e, v3(0, 0, 30)); /* out of the way */

    /* group and ungroup */
    ed_select(e, b, false);
    ed_select(e, c, true);
    ed_group(e);
    i32 m = ed_first(e);
    CHECK(m >= 0 && e->world.inst[m].cls == UB_CLASS_MODEL && e->world.inst[b].parent == m && e->world.inst[c].parent == m);
    vec3 lo, hi;
    CHECK(ed_selection_bounds(e, &lo, &hi) && hi.x - lo.x > 3.9f);
    /* picking a part inside the model picks the model (or the part) */
    vec3 hp;
    vec3 target = part_of(e, b)->pos;
    i32 picked = ed_pick(e, v3(target.x, 50, target.z), v3(0, -1, 0), true, &hp);
    CHECK(picked == m && fabsf(hp.y - (target.y + 0.5f)) < 1e-3f);
    CHECK(ed_pick(e, v3(target.x, 50, target.z), v3(0, -1, 0), false, &hp) == b);
    ed_ungroup(e);
    CHECK(!ub_world_alive(&e->world, m) && e->sel_count == 2 && e->world.inst[b].parent == e->world.workspace);

    /* delete: services and the Workspace stay */
    ed_select(e, e->world.workspace, false);
    ed_delete(e);
    CHECK(ub_world_alive(&e->world, e->world.workspace));
    ed_select(e, b, false);
    int live = ub_world_live_count(&e->world);
    ed_delete(e);
    CHECK(!ub_world_alive(&e->world, b) && ub_world_live_count(&e->world) == live - 1);

    /* scripts: inserted into the selection; behaviours once per part */
    ed_select(e, c, false);
    i32 s = ed_insert(e, ED_INS_SCRIPT, v3(0, 0, 0));
    CHECK(s >= 0 && e->world.inst[s].parent == c && ub_world_source(&e->world, s)[0]);
    ed_select(e, c, false);
    CHECK(ed_add_behaviour(e, ED_BEH_KILL) == 1 && ed_add_behaviour(e, ED_BEH_KILL) == 0);
    CHECK(ub_world_find_child(&e->world, c, "KillBrick") >= 0 && part_of(e, c)->color == 0xFF3A1C);
    ed_set_name(e, c, "Lava");
    CHECK(strcmp(ub_world_str(&e->world, e->world.inst[c].name), "Lava") == 0);
    ed_set_source(e, s, "print('changed')");
    CHECK(strcmp(ub_world_source(&e->world, s), "print('changed')") == 0);

    /* save and load round-trip */
    u8 *data;
    u32 n;
    CHECK(ed_save(e, &data, &n) && !e->dirty);
    ub_editor e2;
    ed_init(&e2);
    CHECK(ed_load(&e2, data, n, false) == UB_LOAD_OK);
    i32 c2 = ub_world_find_child(&e2.world, e2.world.workspace, "Lava");
    CHECK(c2 >= 0 && ub_world_find_child(&e2.world, c2, "KillBrick") >= 0);
    free(data);
    ed_free(&e2);

    /* new place: history gone */
    ed_new_place(e);
    CHECK(!ed_undo_name(e) && e->sel_count == 0);
    ed_free(e);
}

/* ---------------------------------------------------- the exit test */

/* Places a block at a Roblox position with a Roblox size, with editor
 * actions (insert, then type its numbers into the properties). */
static i32 block(ub_editor *e, vec3 rbx_pos, vec3 size)
{
    i32 b = ed_insert(e, ED_INS_BLOCK, v3(0, 0, 0));
    ed_set_rbx(e, b, rbx_pos, v3(0, 0, 0), size);
    return b;
}

static int output_has(const char *needle)
{
    const char *lines[UB_SCRIPT_OUTPUT_LINES];
    bool err[UB_SCRIPT_OUTPUT_LINES];
    int n = ub_script_output(lines, err, UB_SCRIPT_OUTPUT_LINES);
    for (int i = 0; i < n; ++i)
        if (strstr(lines[i], needle)) return err[i] ? -1 : 1;
    return 0;
}

static ub_game g_game;

static void test_obstacle_course(void)
{
    ub_editor *e = &g_ed;
    ed_init(e);
    i32 ws = e->world.workspace;
    /* the floor is lava */
    ed_select(e, ub_world_find_child(&e->world, ws, "Baseplate"), false);
    CHECK(ed_add_behaviour(e, ED_BEH_KILL) == 1);
    /* platforms over the lava, heading away from the spawn (Roblox -Z),
     * 6 studs long with 3-stud gaps */
    i32 p1 = block(e, v3(0, 0.5f, -9), v3(6, 1, 6));
    i32 p2 = block(e, v3(0, 0.5f, -18), v3(6, 1, 6));
    CHECK(ed_add_behaviour(e, ED_BEH_CHECKPOINT) == 1);
    i32 p3 = block(e, v3(0, 0.5f, -27), v3(6, 1, 6));
    CHECK(ed_add_behaviour(e, ED_BEH_FINISH) == 1);
    /* save and load, as a player would, then play */
    u8 *data;
    u32 n;
    CHECK(ed_save(e, &data, &n));
    ub_editor e2;
    ed_init(&e2);
    CHECK(ed_load(&e2, data, n, false) == UB_LOAD_OK);
    free(data);
    ub_world w;
    CHECK(ed_play_world(&e2, &w));
    ed_free(&e2);
    CHECK(ub_game_start(&g_game, &w, true));
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.scripts == 3);
    (void)p1;
    (void)p2;
    (void)p3;

    /* the player: walk on, jump near the end of each platform */
    ub_game *g = &g_game;
    int deaths = 0;
    bool was_dead = false, checkpoint_done = false, died_after_checkpoint = false;
    for (int f = 0; f < 60 * 20 && output_has("finished the course") == 0; ++f) {
        bool jump = false;
        if (g->chr.grounded && g->chr.ground_part >= 0) {
            const ub_part *gp = &g->world.parts[g->chr.ground_part];
            f32 far_edge = gp->pos.z + gp->size.z * 0.5f; /* Uniblox +z is Roblox -z */
            jump = g->feet.z > far_edge - 1.2f;
        }
        ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 1), jump);
        if (g->dead && !was_dead) deaths++;
        was_dead = g->dead;
        /* once at the checkpoint, fall in the lava on purpose: we must come back there */
        if (!checkpoint_done && output_has("Checkpoint reached") == 1) {
            checkpoint_done = true;
            ub_game_teleport(g, v3(10, 0.1f, 18));
            for (int k = 0; k < 60 * 5 && !(died_after_checkpoint = g->dead); ++k)
                ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
            for (int k = 0; k < 60 * 5 && g->dead; ++k) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
            printf("  course: died in the lava, came back at z = %.1f (checkpoint at 18)\n", (double)g->feet.z);
            CHECK(fabsf(g->feet.z - 18) < 1 && fabsf(g->feet.y - 1) < 0.2f);
            was_dead = false;
        }
    }
    printf("  course: checkpoint %s, finish %s, %d death(s) on the way\n", checkpoint_done ? "reached" : "missed",
           output_has("finished the course") == 1 ? "reached" : "missed", deaths);
    CHECK(checkpoint_done && died_after_checkpoint);
    CHECK(output_has("finished the course") == 1);
    CHECK(deaths == 0);
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    ub_game_stop(g);
    ed_free(e);
}

/* The other behaviours do their jobs. */
static void test_behaviours(void)
{
    ub_editor *e = &g_ed;
    ed_init(e);
    i32 pad = block(e, v3(20, 0.5f, 0), v3(6, 1, 6));
    ed_add_behaviour(e, ED_BEH_BOUNCE);
    i32 speed = block(e, v3(40, 0.5f, 0), v3(6, 1, 6));
    ed_add_behaviour(e, ED_BEH_SPEED);
    i32 spin = block(e, v3(0, 10, 30), v3(8, 1, 1));
    ed_add_behaviour(e, ED_BEH_SPIN);
    i32 mover = block(e, v3(-20, 3, 0), v3(4, 1, 4));
    ed_add_behaviour(e, ED_BEH_MOVE);
    i32 gone = block(e, v3(0, 0.5f, 40), v3(6, 1, 6));
    ed_add_behaviour(e, ED_BEH_DISAPPEAR);
    i32 door = block(e, v3(60, 4, 0), v3(1, 8, 6));
    ed_add_behaviour(e, ED_BEH_DOOR);
    (void)pad; (void)speed; (void)spin; (void)mover; (void)gone; (void)door;
    ub_world w;
    CHECK(ed_play_world(e, &w));
    CHECK(ub_game_start(&g_game, &w, true));
    ub_game *g = &g_game;
    i32 ws = g->world.workspace;
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.scripts == 6);
    /* find them in the game's copy (same order: inserted after the spawn) */
    i32 parts[8], np = 0;
    for (i32 c = g->world.inst[ws].first_child; c >= 0 && np < 8; c = g->world.inst[c].next_sibling)
        if (!strcmp(ub_world_str(&g->world, g->world.inst[c].name), "Part")) parts[np++] = c;
    CHECK(np == 6);
    #define GP(i) (&g->world.parts[g->world.inst[parts[i]].part])

    /* bounce: stepping on the pad throws us up */
    ub_game_teleport(g, v3(20, 1.2f, 0));
    f32 top = 0;
    for (int f = 0; f < 60; ++f) {
        ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
        if (g->feet.y > top) top = g->feet.y;
    }
    printf("  bounce pad: thrown %.1f studs up\n", (double)top);
    CHECK(top > 15);
    /* speed pad */
    for (int f = 0; f < 120 && !g->chr.grounded; ++f) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    ub_game_teleport(g, v3(40, 1.2f, 0));
    for (int f = 0; f < 10; ++f) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK(g->walk_speed == 40);
    for (int f = 0; f < 60 * 4; ++f) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK(g->walk_speed == 16);
    /* spinner turned, mover moved */
    CHECK(fabsf(GP(2)->rot.x.x - 1) > 0.01f);
    CHECK(fabsf(GP(3)->pos.x - (-20)) > 1);
    /* disappearing platform: step on, it goes, then comes back */
    ub_game_teleport(g, v3(0, 1.2f, -40));
    for (int f = 0; f < 60; ++f) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK(!(GP(4)->flags & UB_PART_CAN_COLLIDE) && GP(4)->transparency > 200);
    ub_game_teleport(g, v3(-40, 0.1f, 0)); /* (else we would be standing in it when it comes back) */
    for (int f = 0; f < 60 * 4; ++f) ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK((GP(4)->flags & UB_PART_CAN_COLLIDE) && GP(4)->transparency == 0);
    /* door */
    ub_game_teleport(g, v3(55, 0.1f, 0));
    for (int f = 0; f < 60 && GP(5)->transparency == 0; ++f) ub_game_update(g, 1.0f / 60.0f, v3(1, 0, 0), false);
    CHECK(GP(5)->transparency > 100 && !(GP(5)->flags & UB_PART_CAN_COLLIDE));
    #undef GP
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    ub_game_stop(g);
    ed_free(e);
}

void test_editor(void)
{
    test_basics();
    test_obstacle_course();
    test_behaviours();
}
