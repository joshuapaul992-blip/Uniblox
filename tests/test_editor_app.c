/* Uniblox - the editor app (app_editor.c) driven through its own UI with
 * made-up mouse and keyboard input, on the software renderer: build a
 * small course without writing code, play it, save it, open it again. */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/app.h"
#include "app/app_editor.h"
#include "render/ub_render.h"
#include "script/ub_script.h"
#include "ui/ub_ui.h"
#include "test.h"

#define EW 1024
#define EH 640

static u32 g_px[EW * EH];
static u8 g_px8[EW * EH];
static ub_framebuffer g_fb;
static ub_frame g_frame;
static ub_input g_in;

static void frame(void)
{
    app_frame(&g_frame, &g_in, 16667);
    memset(g_in.key_pressed, 0, sizeof g_in.key_pressed);
    memset(g_in.key_repeat, 0, sizeof g_in.key_repeat);
    memset(g_in.mouse_pressed, 0, sizeof g_in.mouse_pressed);
    memset(g_in.mouse_released, 0, sizeof g_in.mouse_released);
    g_in.text[0] = 0;
    g_in.wheel = 0;
}

static void frames(int n)
{
    for (int i = 0; i < n; ++i) frame();
}

static void press_at(int x, int y)
{
    g_in.mouse_x = x;
    g_in.mouse_y = y;
    frame();
    g_in.mouse_down[0] = g_in.mouse_pressed[0] = true;
    frame();
}

static void release(void)
{
    g_in.mouse_down[0] = false;
    g_in.mouse_released[0] = true;
    frame();
}

static void click_at(int x, int y)
{
    press_at(x, y);
    release();
    frame(); /* let the panels show what the click did */
}

static bool click(const char *label)
{
    ui_rect r;
    if (!ui_find(label, &r)) {
        printf("  (no \"%s\" on screen)\n", label);
        return false;
    }
    click_at(r.x + r.w / 2, r.y + r.h / 2);
    return true;
}

static void key(int k, bool with_ctrl)
{
    g_in.key_down[UB_KEY_CONTROL] = with_ctrl;
    g_in.key_down[k] = true;
    g_in.key_pressed[k] = g_in.key_repeat[k] = true;
    frame();
    g_in.key_down[k] = false;
    g_in.key_down[UB_KEY_CONTROL] = false;
}

static void type(const char *s)
{
    snprintf(g_in.text, sizeof g_in.text, "%s", s);
    frame();
}

static bool part_on_screen(i32 inst, int *x, int *y)
{
    ub_editor *e = app_editor_model();
    const ub_part *p = &e->world.parts[e->world.inst[inst].part];
    f32 sx, sy;
    if (!ub_render_project(app_editor_camera(), EW, EH, p->pos, &sx, &sy)) return false;
    *x = (int)sx;
    *y = (int)sy;
    return true;
}

static i32 count_class(const char *cls)
{
    ub_editor *e = app_editor_model();
    i32 n = 0;
    for (i32 i = 0; i < e->world.count; ++i)
        if (ub_world_alive(&e->world, i) && !strcmp(ub_world_str(&e->world, e->world.inst[i].class_name), cls)) n++;
    return n;
}

void test_editor_app(void)
{
    remove("build/host/files/places/course.ubx");
    memset(&g_in, 0, sizeof g_in);
    g_in.has_focus = true;
    g_fb.format = UB_PIXEL_XRGB32;
    g_fb.pixels = g_px;
    g_fb.pixels8 = g_px8;
    g_fb.width = EW;
    g_fb.height = EH;
    g_fb.pitch = EW;
    memset(&g_frame, 0, sizeof g_frame);
    g_frame.backend = UB_BACKEND_SOFTWARE;
    g_frame.width = EW;
    g_frame.height = EH;
    g_frame.fb = &g_fb;
    ub_display_prefs prefs;
    memset(&prefs, 0, sizeof prefs);
    app_preinit(&prefs);
    ub_system_info sys;
    memset(&sys, 0, sizeof sys);
    CHECK(app_init(&sys, &g_frame));
    frames(3);
    ub_editor *e = app_editor_model();
    i32 parts0 = e->world.part_count;

    /* the toolbar inserts a block, selected, in front of the camera */
    CHECK(click("+ Block"));
    CHECK(e->world.part_count == parts0 + 1 && e->sel_count == 1);
    i32 block = ed_first(e);
    CHECK(block >= 0 && e->world.inst[block].part >= 0);
    /* a behaviour from the properties panel: no code written */
    CHECK(click("Add behaviour") && click("Kill brick"));
    CHECK(ub_world_find_child(&e->world, block, "KillBrick") >= 0);
    /* the Behaviours menu does the same */
    CHECK(click("Behaviours") && click("Spinner"));
    CHECK(ub_world_find_child(&e->world, block, "Spinner") >= 0);

    /* type its size into the properties */
    ui_rect r;
    CHECK(ui_find("##sx", &r));
    click_at(r.x + r.w / 2, r.y + r.h / 2);
    type("10\r");
    vec3 pos, ori, size;
    ed_get_rbx(e, block, &pos, &ori, &size);
    CHECK(fabsf(size.x - 10) < 1e-4f);

    /* drag it along the ground in the 3D view */
    int bx, by;
    CHECK(part_on_screen(block, &bx, &by));
    vec3 before = e->world.parts[e->world.inst[block].part].pos;
    press_at(bx, by);
    for (int k = 1; k <= 10; ++k) {
        g_in.mouse_x = bx + k * 12;
        frame();
    }
    release();
    vec3 after = e->world.parts[e->world.inst[block].part].pos;
    printf("  editor: dragged the block %.1f studs\n", (double)v3_length(v3_sub(after, before)));
    CHECK(v3_length(v3_sub(after, before)) > 2 && fabsf(after.y - before.y) < 1e-4f);
    CHECK(fabsf(after.x - floorf(after.x + 0.5f)) < 1e-4f); /* on the grid */
    /* undo it with Ctrl+Z */
    key('Z', true);
    after = e->world.parts[e->world.inst[ed_first(e)].part].pos;
    CHECK(v3_length(v3_sub(after, before)) < 1e-4f);
    block = ed_first(e);

    /* the Move tool's +x arrow, then the Scale tool's +x face arrow */
    for (int tool = 0; tool < 2; ++tool) {
        key(tool == 0 ? '2' : '3', false);
        frames(2);
        const ub_part *bp = &e->world.parts[e->world.inst[block].part];
        vec3 lo, hi;
        CHECK(ed_selection_bounds(e, &lo, &hi));
        vec3 c = tool == 0 ? v3_scale(v3_add(lo, hi), 0.5f) : bp->pos;
        f32 half = tool == 0 ? (hi.x - lo.x) * 0.5f + 0.3f : bp->size.x * 0.5f + 0.2f;
        f32 len = v3_length(v3_sub(c, app_editor_camera()->pos)) * 0.07f;
        len = len < 0.8f ? 0.8f : len > 8 ? 8 : len;
        vec3 mid = v3_add(c, v3(half + len * 0.5f, 0, 0)), far = v3_add(mid, v3(5, 0, 0));
        f32 mx, my, fx, fy;
        CHECK(ub_render_project(app_editor_camera(), EW, EH, mid, &mx, &my));
        CHECK(ub_render_project(app_editor_camera(), EW, EH, far, &fx, &fy));
        f32 dl = sqrtf((fx - mx) * (fx - mx) + (fy - my) * (fy - my));
        vec3 p0 = bp->pos, s0 = bp->size;
        press_at((int)mx, (int)my);
        for (int k = 1; k <= 10; ++k) {
            g_in.mouse_x = (int)(mx + (fx - mx) / dl * 8.0f * (f32)k);
            g_in.mouse_y = (int)(my + (fy - my) / dl * 8.0f * (f32)k);
            frame();
        }
        release();
        bp = &e->world.parts[e->world.inst[block].part];
        if (tool == 0) {
            printf("  editor: move arrow slid it %.1f studs along x\n", (double)(bp->pos.x - p0.x));
            CHECK(bp->pos.x - p0.x >= 1 && fabsf(bp->pos.z - p0.z) < 1e-4f && fabsf(bp->size.x - s0.x) < 1e-4f);
        } else {
            printf("  editor: scale arrow grew it %.1f studs (moved %.1f)\n", (double)(bp->size.x - s0.x), (double)(bp->pos.x - p0.x));
            CHECK(bp->size.x - s0.x >= 1 && fabsf((bp->pos.x - p0.x) * 2 - (bp->size.x - s0.x)) < 1e-3f);
        }
    }

    /* insert from the menu */
    CHECK(click("Insert") && click("Cylinder"));
    CHECK(count_class("Part") >= 3);
    key(UB_KEY_DELETE, false);
    key('1', false);

    /* select the block in the explorer; open its script by double-clicking it */
    CHECK(click("Workspace"));
    frames(2);
    char lab[64];
    snprintf(lab, sizeof lab, "Part##%d", block);
    CHECK(click(lab) && ed_first(e) == block);
    frames(2);
    i32 kill = ub_world_find_child(&e->world, block, "KillBrick");
    CHECK(click("Edit KillBrick"));
    frames(2);
    CHECK(ui_find("script text", &r));
    type("-- my lava\r");
    CHECK(click("Save and close"));
    CHECK(!strncmp(ub_world_source(&e->world, kill), "-- my lava\n", 11));

    /* play it: the character appears, scripts run; F5 stops */
    CHECK(click("Play (F5)"));
    CHECK(app_editor_playing());
    frames(30);
    ub_script_stats st;
    CHECK(app_editor_server() && ub_game_scripts(app_editor_server())); /* (the play test is networked) */
    ub_script_get_stats(&st);
    CHECK(st.scripts == 2 && st.errors == 0);
    CHECK(app_editor_game()->char_model >= 0);
    key(UB_KEY_F5, false);
    CHECK(!app_editor_playing());
    frames(2);

    /* save as, through the dialog */
    CHECK(click("File") && click("Save as..."));
    CHECK(click("File name"));
    type("\b");
    type("course\r");
    CHECK(click("Save##as"));
    printf("  editor: %s\n", app_editor_status());
    FILE *f = fopen("build/host/files/places/course.ubx", "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    CHECK(!e->dirty && !strcmp(e->world.name, "course"));

    /* a new place, then open the saved one from the list */
    CHECK(click("File") && click("New place"));
    CHECK(e->world.part_count == parts0);
    CHECK(click("File") && click("Open..."));
    CHECK(click("places/course.ubx"));
    i32 found = -1;
    for (i32 i = 0; i < e->world.count; ++i)
        if (ub_world_alive(&e->world, i) && ub_world_find_child(&e->world, i, "KillBrick") >= 0) found = i;
    CHECK(found >= 0);
    CHECK(found >= 0 && !strncmp(ub_world_source(&e->world, ub_world_find_child(&e->world, found, "KillBrick")), "-- my lava", 10));

    /* unsaved changes: New asks first */
    click("+ Ball");
    CHECK(e->dirty);
    CHECK(click("File") && click("New place"));
    CHECK(ui_find("Don't save", &r));
    CHECK(click("Cancel"));
    CHECK(e->dirty && e->world.part_count == parts0 + 2);

    /* a big place stays usable: 20,000 parts, select all, move, undo */
    i32 ws = e->world.workspace;
    for (int i = 0; i < 20000; ++i) {
        ub_part p;
        memset(&p, 0, sizeof p);
        p.pos = v3((f32)(i % 140) * 3, 0.5f, (f32)(i / 140) * 3 + 20);
        p.rot = mat3_identity();
        p.size = v3(2, 1, 2);
        p.color = 0x808080;
        p.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
        ub_world_add_part(&e->world, ws, UB_CLASS_PART, NULL, "Part", &p);
    }
    e->scene_version++;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    frame();
    key('A', true);
    frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double select_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(e->sel_count >= 20000);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    frames(3);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double frame_ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 3;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ed_checkpoint(e, "Move");
    ed_translate(e, v3(0, 5, 0));
    frame();
    key('Z', true);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double undo_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("  editor, 20,000 parts: select all %.0f ms, a frame with all selected %.0f ms, move + undo %.0f ms\n",
           select_ms, frame_ms, undo_ms);
    CHECK(select_ms < 3000 && frame_ms < 1500 && undo_ms < 3000);
    CHECK(e->sel_count >= 20000);

    app_shutdown();
}
