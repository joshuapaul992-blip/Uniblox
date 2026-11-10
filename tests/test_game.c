/* Uniblox - whole games run headless: scripts, the character and physics
 * together. The M5 exit test is here: a touch-activated door works, and
 * hostile scripts are contained. */
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <time.h>

#include <math.h>
#include "app/app_play.h"
#include "game/ub_demo_place.h"
#include "game/ub_game.h"
#include "gui/ub_gui.h"
#include "physics/ub_hull.h"
#include "world/ub_classes.h"
#include "render/ub_texture.h"
#include "script/ub_script.h"
#include "test.h"

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static i32 add_box(ub_world *w, i32 parent, const char *name, vec3 pos, vec3 size, u32 color)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = pos;
    p.rot = mat3_identity();
    p.size = size;
    p.color = color;
    p.shape = UB_SHAPE_BOX;
    p.material = UB_MAT_PLASTIC;
    p.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    return ub_world_add_part(w, parent, UB_CLASS_PART, NULL, name, &p);
}

static i32 add_script(ub_world *w, i32 parent, const char *name, const char *src)
{
    i32 s = ub_world_add(w, parent, UB_CLASS_OTHER, "Script", name);
    if (s >= 0) ub_world_set_source(w, s, src);
    return s;
}

/* A floor, a spawn at the origin, and a wall across +z at z = 10 with a
 * doorway (x -2..2) that a Door part fills. */
static void door_world(ub_world *w, const char *door_script)
{
    ub_world_init(w);
    i32 ws = ub_world_workspace(w);
    add_box(w, ws, "Floor", v3(0, -0.5f, 10), v3(60, 1, 60), 0x4C8C3C);
    ub_part sp;
    memset(&sp, 0, sizeof sp);
    sp.pos = v3(0, 0.05f, 0);
    sp.rot = mat3_identity();
    sp.size = v3(4, 0.1f, 4);
    sp.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    ub_world_add_part(w, ws, UB_CLASS_SPAWN, NULL, "SpawnLocation", &sp);
    add_box(w, ws, "WallLeft", v3(-16, 5, 10), v3(28, 10, 1), 0xB04A30);
    add_box(w, ws, "WallRight", v3(16, 5, 10), v3(28, 10, 1), 0xB04A30);
    add_box(w, ws, "WallTop", v3(0, 8, 10), v3(4, 4, 1), 0xB04A30);
    i32 door = add_box(w, ws, "Door", v3(0, 3, 10), v3(4, 6, 1), 0x6B4423);
    if (door_script) add_script(w, door, "DoorScript", door_script);
}

static const char k_door[] =
    "local door = script.Parent\n"
    "local open = false\n"
    "door.Touched:Connect(function(hit)\n"
    "  if open then return end\n"
    "  local humanoid = hit.Parent:FindFirstChildOfClass('Humanoid')\n"
    "  if not humanoid then return end\n"
    "  open = true\n"
    "  door.Transparency = 0.6\n"
    "  door.CanCollide = false\n"
    "  print('door opened by ' .. hit.Parent.Name)\n"
    "  task.wait(3)\n"
    "  door.Transparency = 0\n"
    "  door.CanCollide = true\n"
    "  open = false\n"
    "end)\n";

static ub_game g_g;

/* Walks towards +z for `seconds`; returns the farthest z reached. */
static f32 walk_watching(ub_game *g, f32 seconds, f32 *door_transparency_seen, i32 door)
{
    f32 best = -1e9f;
    for (int i = 0; i < (int)(seconds * 60); ++i) {
        ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 1), false);
        if (g->feet.z > best) best = g->feet.z;
        if (door_transparency_seen && door >= 0) {
            u8 t = g->world.parts[g->world.inst[door].part].transparency;
            if (t > *door_transparency_seen) *door_transparency_seen = t;
        }
    }
    return best;
}

static f32 walk(ub_game *g, f32 seconds, f32 *door_transparency_seen)
{
    return walk_watching(g, seconds, door_transparency_seen,
                         ub_world_find_child(&g->world, ub_world_workspace(&g->world), "Door"));
}

static void test_door(void)
{
    /* no script: the door is a wall */
    ub_world w;
    door_world(&w, NULL);
    CHECK(ub_game_start(&g_g, &w, true));
    f32 z = walk(&g_g, 3.0f, NULL);
    CHECK(z < 10);
    ub_game_stop(&g_g);

    /* with the script: touching it opens it, the player walks through,
     * then it closes again */
    door_world(&w, k_door);
    CHECK(ub_game_start(&g_g, &w, true));
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.scripts == 1);
    f32 seen = 0;
    z = walk(&g_g, 3.0f, &seen);
    printf("  door: walked to z = %.1f, door transparency reached %d/255\n", (double)z, (int)seen);
    CHECK(z > 12);
    CHECK(seen > 140 && seen < 160);
    i32 door = ub_world_find_child(&g_g.world, ub_world_workspace(&g_g.world), "Door");
    /* three seconds after opening it is shut again */
    for (int i = 0; i < 240; ++i) ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    const ub_part *dp = &g_g.world.parts[g_g.world.inst[door].part];
    CHECK(dp->transparency == 0 && (dp->flags & UB_PART_CAN_COLLIDE));
    const char *lines[8];
    bool err[8];
    int n = ub_script_output(lines, err, 8);
    bool said = false;
    for (int i = 0; i < n; ++i) said |= strcmp(lines[i], "door opened by Player1") == 0 && !err[i];
    CHECK(said);
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    /* walking back: the closed door blocks again */
    for (int i = 0; i < 180; ++i) ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 1), false);
    CHECK(g_g.feet.z > 10.5f);
    ub_game_stop(&g_g);
}

/* --------------------------------------------------------- the API */

static i32 find_result(ub_game *g, const char *name)
{
    return ub_world_find_child(&g->world, ub_world_workspace(&g->world), name);
}

static void print_output(void)
{
    const char *lines[8];
    bool err[8];
    int n = ub_script_output(lines, err, 8);
    for (int i = 0; i < n; ++i) printf("    %s%s\n", err[i] ? "[error] " : "", lines[i]);
}

static void test_api(void)
{
    char *src = read_text("tests/data/scripts/api.lua");
    CHECK(src != NULL);
    if (!src) return;
    ub_world w;
    door_world(&w, NULL);
    i32 ws = ub_world_workspace(&w);
    i32 model = ub_world_add(&w, ws, UB_CLASS_MODEL, NULL, "Stuff");
    add_box(&w, model, "A", v3(20, 1, -5), v3(2, 2, 2), 0xFF0000);
    add_box(&w, model, "B", v3(24, 1, -5), v3(2, 2, 2), 0x00FF00);
    ub_world_add(&w, -1, UB_CLASS_LIGHTING, NULL, "Lighting");
    i32 sss = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    i32 rs = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ReplicatedStorage", "ReplicatedStorage");
    i32 mod = ub_world_add(&w, rs, UB_CLASS_OTHER, "ModuleScript", "Util");
    ub_world_set_source(&w, mod, "local M = {}\nfunction M.double(x) return x * 2 end\nM.loaded = (M.loaded or 0) + 1\nreturn M\n");
    add_script(&w, sss, "Api", src);
    free(src);
    CHECK(ub_game_start(&g_g, &w, true));
    for (int i = 0; i < 600 && find_result(&g_g, "api done") < 0; ++i)
        ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    bool ok = find_result(&g_g, "api ok") >= 0;
    CHECK(find_result(&g_g, "api done") >= 0);
    CHECK(ok);
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    if (!ok || st.errors) print_output();
    ub_game_stop(&g_g);
}

/* Luau's library additions (bit32, utf8, DateTime, JSON...). */
static void test_luaulib(void)
{
    char *src = read_text("tests/data/scripts/luaulib.lua");
    CHECK(src != NULL);
    if (!src) return;
    ub_world w;
    door_world(&w, NULL);
    i32 sss = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    add_script(&w, sss, "Lib", src);
    /* attributes and tags as a place file brings them */
    i32 tagged = ub_world_add(&w, ub_world_workspace(&w), UB_CLASS_FOLDER, "Folder", "Tagged");
    ub_value speed;
    memset(&speed, 0, sizeof speed);
    speed.type = UB_PV_DOUBLE;
    speed.v.d = 16;
    ub_value tags = ub_vstr("Coin\0Spin", 9);
    CHECK(ub_world_set_prop_named(&w, tagged, "@Speed", &speed) && ub_world_set_prop_named(&w, tagged, "Tags", &tags));
    free(src);
    CHECK(ub_game_start(&g_g, &w, true));
    for (int i = 0; i < 300 && find_result(&g_g, "lib done") < 0; ++i)
        ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    bool ok = find_result(&g_g, "lib ok") >= 0;
    CHECK(find_result(&g_g, "lib done") >= 0);
    CHECK(ok);
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    if (!ok || st.errors) print_output();
    ub_game_stop(&g_g);
}

/* ------------------------------------------------------ hostile code */

static const char k_survivor[] =
    "local v = Instance.new('IntValue')\n"
    "v.Name = 'Beats'\n"
    "v.Parent = workspace\n"
    "game:GetService('RunService').Heartbeat:Connect(function() v.Value += 1 end)\n";

static f64 now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (f64)t.tv_sec + (f64)t.tv_nsec * 1e-9;
}

typedef struct {
    const char *what;
    const char *src;
    bool stopped;        /* expect it to be stopped for running too long */
} hostile;

static const hostile k_hostile[] = {
    { "busy loop", "while true do end", true },
    { "busy loop catching errors", "while true do pcall(function() while true do end end) end", true },
    { "busy loop in a coroutine", "coroutine.wrap(function() while true do end end)()", true },
    { "busy loop in an event", "workspace.ChildAdded:Connect(function() while true do end end)\n"
                               "Instance.new('Folder', workspace)", true },
    { "busy loop after waiting", "wait(0.1) local x = 0 while true do x = x + 1 end", true },
    { "recursion", "local function f() return f() + 1 end f()", false },
    { "memory bomb", "local t = {} local s = 'x' while true do s = s .. s t[#t + 1] = s end", false },
    { "table bomb", "local t = {} for i = 1, 1e9 do t[i] = {i} end", false },
    { "pattern bomb", "string.rep('a', 5000):match(string.rep('a?', 5000) .. string.rep('a', 5000))", false },
    { "nested pattern", "('a'):rep(100000):find(('.-'):rep(200) .. 'b')", false },
    { "string.rep bomb", "local s = string.rep('x', 2^30)", false },
    { "gsub bomb", "local s = ('x'):rep(1e6) for i = 1, 30 do s = s:gsub('x', 'xx') end", false },
    { "format bomb", "local s = string.format('%099999d', 1)", false },
    { "no loadstring", "assert(loadstring == nil and load == nil and dofile == nil and loadfile == nil)\n"
                       "assert(getfenv == nil and setfenv == nil and debug == nil and io == nil)\n"
                       "assert(string.dump == nil and newproxy == nil and module == nil)\n"
                       "assert(os.execute == nil and os.remove == nil and os.exit == nil and os.getenv == nil)\n"
                       "error('checked')", false },
    { "require a Script", "require(script)", false },
    { "string metatable", "getmetatable('').__index = {}", false },
    { "instance metatable", "local mt = getmetatable(workspace) mt.__index = nil", false },
    { "rawset on library", "rawset(math, 'floor', nil) rawset(string, 'len', nil) error('checked')", false },
    { "global tampering", "print = nil wait = nil Instance = nil game = nil math.floor = nil string.len = nil "
                          "_G.x = 1 shared.y = 2 error('checked')", false },
    { "yield across C", "table.sort({3, 2, 1}, function(a, b) wait() return a < b end)", false },
    { "error objects", "error(setmetatable({}, {__tostring = function() while true do end end}))", false },
    { "printing error objects", "print(setmetatable({}, {__tostring = function() while true do end end}))", true },
    { "busy loop in pcall'd wait", "pcall(function() wait() while true do end end)", true },
    { "catching out of memory", "local t = {} while true do pcall(function() for i = 1, 1e6 do t[#t + 1] = {} end end) end",
      true },
    { "destroy services", "for _, s in ipairs(game:GetChildren()) do pcall(function() s:Destroy() end) end "
                          "workspace.Parent = nil", false },
    { "destroy character", "workspace.Player1:Destroy() workspace.Player1.Humanoid:Destroy() error('checked')",
      false },
    { "many threads", "for i = 1, 100000 do task.spawn(function() wait(1e9) end) end", false },
    { "many instances", "for i = 1, 300000 do Instance.new('Part') end", false },
};

static void test_hostile(void)
{
    ub_world w;
    door_world(&w, k_door);
    i32 sss = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    add_script(&w, sss, "Survivor", k_survivor);
    CHECK(ub_game_start(&g_g, &w, true));
    ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    i32 beats_inst = find_result(&g_g, "Beats");
    CHECK(beats_inst >= 0);
    ub_script_stats st0, st;
    for (int h = 0; h < (int)(sizeof k_hostile / sizeof k_hostile[0]); ++h) {
        ub_script_get_stats(&st0);
        f64 t0 = now_s();
        bool compiled = ub_script_run(k_hostile[h].what, k_hostile[h].src, ub_world_workspace(&g_g.world));
        for (int i = 0; i < 12; ++i) ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
        f64 took = now_s() - t0;
        ub_script_get_stats(&st);
        bool stopped = st.stopped > st0.stopped, errored = st.errors > st0.errors;
        printf("  hostile: %-28s %5.0f ms, %s, %u KB\n", k_hostile[h].what, took * 1000.0,
               stopped ? "stopped" : errored ? "error" : "ran", st.memory_kb);
        CHECK(compiled);
        if (!compiled) print_output();
        CHECK(took < 3.0);
        if (k_hostile[h].stopped) CHECK(stopped);
        else CHECK(errored || stopped);
        CHECK(st.memory_kb <= UB_SCRIPT_MEMORY / 1024);
    }
    /* precompiled code is refused */
    CHECK(!ub_script_run("bytecode", "\033LuaQ\0\1\4\4\4\x08", ub_world_workspace(&g_g.world)));
    /* the game, the door and the other scripts carry on */
    CHECK(ub_world_alive(&g_g.world, g_g.char_model) && ub_world_alive(&g_g.world, g_g.humanoid));
    CHECK(ub_world_alive(&g_g.world, ub_world_workspace(&g_g.world)));
    bool ok = ub_script_run("check survivor",
                            "local n = workspace.Beats.Value wait(0.2) assert(workspace.Beats.Value > n)\n"
                            "assert(print and wait and Instance and game and math.floor and string.len)\n"
                            "assert(('abc'):len() == 3 and _G.x == 1)\n"
                            "Instance.new('Folder', workspace).Name = 'survived'",
                            ub_world_workspace(&g_g.world));
    CHECK(ok);
    for (int i = 0; i < 30; ++i) ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK(find_result(&g_g, "survived") >= 0);
    if (find_result(&g_g, "survived") < 0) print_output();
    f32 z = walk(&g_g, 3.0f, NULL);
    CHECK(z > 12); /* the door still opens */
    ub_game_stop(&g_g);
}

/* A long game makes and destroys far more instances than the limit (shots,
 * effects): slots are reused, so it keeps working and memory stays put. */
static void test_reuse(void)
{
    ub_world w;
    door_world(&w, NULL);
    CHECK(ub_game_start(&g_g, &w, true));
    i32 count0 = g_g.world.count;
    CHECK(ub_script_run("bullets",
                        "for i = 1, 150000 do\n"
                        "  local p = Instance.new('Part')\n"
                        "  p.Size = Vector3.new(1, 1, 1)\n"
                        "  p.Position = Vector3.new(i % 20, 5, -20)\n"
                        "  p.Touched:Connect(function() p:Destroy() end)\n"
                        "  p.Parent = workspace\n"
                        "  p:Destroy()\n"
                        "  local q = Instance.new('Folder') -- dropped without a parent: collected\n"
                        "  if i % 250 == 0 then wait() end\n"
                        "end\n"
                        "Instance.new('Folder', workspace).Name = 'bullets done'\n",
                        ub_world_workspace(&g_g.world)));
    for (int i = 0; i < 5000 && find_result(&g_g, "bullets done") < 0; ++i)
        ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    ub_script_stats st;
    ub_script_get_stats(&st);
    printf("  reuse: 300000 instances made, world grew by %d slots, %d physics bodies, %u KB of script memory\n",
           g_g.world.count - count0, g_g.phys.body_count, st.memory_kb);
    CHECK(find_result(&g_g, "bullets done") >= 0);
    CHECK(st.errors == 0);
    CHECK(g_g.world.count - count0 < 50000);
    CHECK(g_g.phys.body_count < 10000);
    if (find_result(&g_g, "bullets done") < 0 || st.errors) print_output();
    ub_game_stop(&g_g);
}

static void gui_frame(ub_game *g, f32 x, f32 y, bool press, bool release, int key, f64 t);

static void frames(int n, vec3 move)
{
    for (int i = 0; i < n; ++i) ub_game_update(&g_g, 1.0f / 60.0f, move, false);
}

/* The built-in demo place: every one of its scripts does its job. */
static void test_demo_place(void)
{
    ub_world w;
    ub_demo_place(&w);
    CHECK(ub_game_start(&g_g, &w, true));
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.scripts == 7); /* the server's; the panel's LocalScript starts with the first frame */
    i32 ws = ub_world_workspace(&g_g.world);
    i32 house = ub_world_find_child(&g_g.world, ws, "House");
    i32 door = ub_world_find_child(&g_g.world, house, "Door");
    i32 spinner = ub_world_find_child(&g_g.world, house, "Spinner");
    CHECK(door >= 0 && spinner >= 0);
    /* walk from the spawn through the door into the house */
    f32 seen = 0;
    f32 z = walk_watching(&g_g, 2.0f, &seen, door);
    printf("  demo place: walked to z = %.1f through the door (transparency reached %d/255)\n", (double)z, (int)seen);
    CHECK(z > 26 && seen > 100);
    /* the spinner turns */
    const ub_part *sp = &g_g.world.parts[g_g.world.inst[spinner].part];
    CHECK(fabsf(sp->rot.x.x - 1.0f) > 0.01f || fabsf(sp->rot.y.y - 1.0f) > 0.01f);
    /* the button rains balls; Debris clears them */
    i32 parts0 = 0, parts1 = 0;
    for (i32 c = g_g.world.inst[ws].first_child; c >= 0; c = g_g.world.inst[c].next_sibling) parts0++;
    ub_game_teleport(&g_g, v3(18, 0.4f, -2));
    frames(30, v3(0, 0, 0));
    for (i32 c = g_g.world.inst[ws].first_child; c >= 0; c = g_g.world.inst[c].next_sibling) parts1++;
    CHECK(parts1 == parts0 + 6);
    frames(60 * 16, v3(0, 0, 0));
    parts1 = 0;
    for (i32 c = g_g.world.inst[ws].first_child; c >= 0; c = g_g.world.inst[c].next_sibling) parts1++;
    CHECK(parts1 == parts0);
    /* the lava kills */
    ub_game_teleport(&g_g, v3(-40, 1.3f, -25));
    frames(20, v3(0, 0, 0));
    CHECK(g_g.dead);
    frames(60 * 4, v3(0, 0, 0));
    CHECK(!g_g.dead);
    /* the panel came with the player, kept its count through the death
     * (ResetOnSpawn off) and is drawn; so are the sign and the door's label */
    ub_script_get_stats(&st);
    CHECK(st.scripts == 8);
    i32 hud = ub_world_find_child(&g_g.world, g_g.player_gui, "DemoHud");
    i32 panel = hud >= 0 ? ub_world_find_child(&g_g.world, hud, "Panel") : -1;
    i32 stats = panel >= 0 ? ub_world_find_child(&g_g.world, panel, "Stats") : -1;
    const ub_value *sv = stats >= 0 ? ub_world_prop_named(&g_g.world, stats, "Text") : NULL;
    CHECK(sv && !strcmp(sv->v.str.s, "Doors opened: 1"));
    if (sv) printf("  demo place: the panel says \"%s\"\n", sv->v.str.s);
    ub_camera cam;
    memset(&cam, 0, sizeof cam);
    cam.pos = v3(0, 4, -8);
    cam.fov_y = 1.2f;
    cam.far_dist = 600;
    gui_frame(&g_g, 400, 400, false, false, 0, 30.0);
    ub_gui_render(g_g.gui, &g_g.world, &cam);
    const int *rects;
    int nr;
    CHECK(ub_gui_screen(g_g.gui, &rects, &nr) != NULL && nr >= 1);
    const ub_gui_quad *q;
    int nq = ub_gui_quads(g_g.gui, &q);
    CHECK(nq == 3); /* the sign, the door's label and the lamp's prompt (the player is back at the spawn) */
    /* drawn by the software renderer over the real scene: the door's label
     * is in front of the wall, so it shows (dark, see-through) */
    {
        static u32 px[320 * 240];
        static u8 px8[320 * 240];
        ub_framebuffer fb;
        memset(&fb, 0, sizeof fb);
        fb.format = UB_PIXEL_XRGB32;
        fb.pixels = px;
        fb.pixels8 = px8;
        fb.width = 320;
        fb.height = 240;
        fb.pitch = 320;
        ub_frame fr;
        memset(&fr, 0, sizeof fr);
        fr.backend = UB_BACKEND_SOFTWARE;
        fr.width = 320;
        fr.height = 240;
        fr.fb = &fb;
        char desc[64];
        CHECK(ub_render_init(&fr, desc, sizeof desc));
        fb.palette = ub_render_palette();
        ub_render_env env = play_env(&g_g.world);
        cam.pos = v3(0, 7, -12);
        cam.pitch = -0.15f;
        ub_render_begin(&fr, &cam, &env);
        for (i32 pi = 0; pi < g_g.world.part_count; ++pi)
            if (ub_game_part_visible(&g_g, pi)) {
                ub_part_draw d = play_part_draw(&g_g.world, &g_g.world.parts[pi]);
                ub_render_part(&d);
            }
        play_gui_input(&g_g, &fr, &(ub_input){ .has_focus = true }, &cam, false, 31.0);
        play_gui_draw_world(&g_g, &cam);
        ub_render_end(NULL);
        nq = ub_gui_quads(g_g.gui, &q);
        f32 sx = 0, sy = 0;
        int found = -1;
        for (int i = 0; i < nq; ++i)
            if (q[i].corners[0].z > 15) found = i; /* the door's, not the sign's */
        CHECK(found >= 0);
        if (found >= 0) {
            vec3 mid = v3_scale(v3_add(q[found].corners[0], q[found].corners[2]), 0.5f);
            CHECK(ub_render_project(&cam, 320, 240, mid, &sx, &sy));
            u32 c = px[(int)sy * 320 + (int)sx];
            int r = (int)(c >> 16 & 255), gg = (int)(c >> 8 & 255), b = (int)(c & 255);
            /* the wall is brick red; the label's panel is a dark blue-grey */
            CHECK(!(r > gg + 40 && r > b + 40));
            if (r > gg + 40 && r > b + 40) printf("    the door's label is hidden: 0x%06x at %d,%d\n", c, (int)sx, (int)sy);
        }
        ub_render_shutdown();
    }
    /* the lamp by the path: E at its prompt switches it on */
    i32 lamp = ub_world_find_child(&g_g.world, ws, "Lamp");
    i32 bulb = lamp >= 0 ? ub_world_find_child(&g_g.world, lamp, "Bulb") : -1;
    CHECK(bulb >= 0);
    gui_frame(&g_g, 400, 400, false, false, 'E', 31.0);
    frames(2, v3(0, 0, 0));
    const ub_value *on = lamp >= 0 ? ub_world_prop_named(&g_g.world, lamp, "@On") : NULL;
    CHECK(on && on->type == UB_PV_BOOL && on->v.b);
    CHECK(bulb >= 0 && (g_g.world.parts[g_g.world.inst[bulb].part].flags & UB_PART_GLOW));
    /* chat commands */
    ub_script_chatted("fast");
    CHECK(g_g.walk_speed == 32);
    ub_script_chatted("night");
    CHECK(g_g.world.lighting.clock_time < 0.01f);
    ub_script_get_stats(&st);
    CHECK(st.errors == 0 && st.stopped == 0);
    if (st.errors) print_output();
    ub_game_stop(&g_g);
}

/* Death and respawn: a kill brick, Died and CharacterAdded. */
static void test_death(void)
{
    ub_world w;
    door_world(&w, NULL);
    i32 ws = ub_world_workspace(&w);
    i32 lava = add_box(&w, ws, "Lava", v3(0, 0.1f, 5), v3(8, 0.2f, 2), 0xFF4000);
    add_script(&w, lava, "Kill",
               "script.Parent.Touched:Connect(function(hit)\n"
               "  local h = hit.Parent:FindFirstChild('Humanoid')\n"
               "  if h then h.Health = 0 end\n"
               "end)\n"
               "local p = game:GetService('Players')\n"
               "p.PlayerAdded:Connect(function(pl)\n"
               "  pl.CharacterAdded:Connect(function(c) Instance.new('Folder', workspace).Name = 'respawned' end)\n"
               "end)\n"
               "workspace.Player1.Humanoid.Died:Connect(function() Instance.new('Folder', workspace).Name = 'died' end)\n");
    CHECK(ub_game_start(&g_g, &w, true));
    walk(&g_g, 1.5f, NULL);
    CHECK(g_g.dead);
    CHECK(find_result(&g_g, "died") >= 0);
    for (int i = 0; i < 200; ++i) ub_game_update(&g_g, 1.0f / 60.0f, v3(0, 0, 0), false);
    CHECK(!g_g.dead && g_g.health == 100);
    CHECK(find_result(&g_g, "respawned") >= 0);
    CHECK(v3_length(v3_sub(g_g.feet, g_g.spawn)) < 1.0f);
    ub_game_stop(&g_g);
}

/* ------------------------------------------------------- the GUI API */

static i32 add_gui(ub_world *w, i32 parent, const char *cls, const char *name)
{
    i32 i = ub_world_add(w, parent, UB_CLASS_OTHER, cls, name);
    ub_cdb_fill_defaults(w, i, cls);
    return i;
}

/* One frame as the game runs it: the GUI's input (mouse at x, y), the
 * game, then the GUI drawn. */
static void gui_frame(ub_game *g, f32 x, f32 y, bool press, bool release, int key, f64 t)
{
    ub_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.backend = UB_BACKEND_SOFTWARE;
    fr.width = 800;
    fr.height = 636;
    static ub_input in;
    memset(&in, 0, sizeof in);
    in.has_focus = true;
    in.mouse_x = (int)x;
    in.mouse_y = (int)y;
    in.mouse_down[0] = press;
    in.mouse_pressed[0] = press;
    in.mouse_released[0] = release;
    if (key) in.key_pressed[key] = in.key_down[key] = in.key_repeat[key] = true;
    play_gui_input(g, &fr, &in, NULL, false, t);
    ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), false);
    ub_gui_render(g->gui, &g->world, NULL);
}

static void test_gui_api(void)
{
    char *src = read_text("tests/data/scripts/gui.lua");
    CHECK(src != NULL);
    if (!src) return;
    ub_world w;
    door_world(&w, NULL);
    i32 sg = ub_world_add(&w, -1, UB_CLASS_FOLDER, "StarterGui", "StarterGui");
    ub_cdb_fill_defaults(&w, sg, "StarterGui");
    i32 hud = add_gui(&w, sg, "ScreenGui", "Hud");
    i32 btn = add_gui(&w, hud, "TextButton", "Btn");
    ub_value v;
    v.type = UB_PV_UDIM2;
    v.v.ud2.sx = 0; v.v.ud2.ox = 100; v.v.ud2.sy = 0; v.v.ud2.oy = 40;
    ub_world_set_prop_named(&w, btn, "Size", &v);
    v.v.ud2.ox = 10; v.v.ud2.oy = 10;
    ub_world_set_prop_named(&w, btn, "Position", &v);
    i32 ls = ub_world_add(&w, hud, UB_CLASS_OTHER, "LocalScript", "GuiTest");
    ub_world_set_source(&w, ls, src);
    w.inst[ls].flags |= UB_INST_CLIENT;
    free(src);
    i32 rs = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ReplicatedStorage", "ReplicatedStorage");
    add_gui(&w, rs, "RemoteEvent", "Ping");
    add_gui(&w, rs, "RemoteFunction", "Ask");
    i32 sss = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    add_script(&w, sss, "Server",
               "local RS = game:GetService('ReplicatedStorage')\n"
               "RS.Ping.OnServerEvent:Connect(function(player, x) RS.Ping:FireClient(player, x * 2) end)\n"
               "RS.Ask.OnServerInvoke = function(player, a, b) return a + b, player.Name end\n");
    CHECK(ub_game_start(&g_g, &w, true));
    /* the screen is 800x636; the button's middle is at (60, 30) in GUI coordinates */
    int ready_at = -1;
    for (int i = 0; i < 900 && find_result(&g_g, "gui done") < 0; ++i) {
        if (ready_at < 0 && find_result(&g_g, "gui ready") >= 0) ready_at = i;
        int k = ready_at >= 0 ? i - ready_at : -1;
        bool on = k >= 2; /* away from the button until the script is ready, then onto it */
        gui_frame(&g_g, on ? 60 : 500, on ? 36 + 30 : 500, k == 5, k == 6, k == 8 ? 'E' : 0, i / 60.0);
    }
    CHECK(find_result(&g_g, "gui done") >= 0);
    bool ok = find_result(&g_g, "gui ok") >= 0;
    CHECK(ok);
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    if (!ok || st.errors) print_output();
    ub_game_stop(&g_g);
}

/* ------------------------------------------------ meshes (8C) */

/* A box piece of a mesh: 8 corners and 12 triangles, appended. */
static void mesh_box(f32 *v, u32 *nv, u32 *tri, u32 *nt, vec3 lo, vec3 hi)
{
    u32 b = *nv;
    for (int i = 0; i < 8; ++i) {
        v[(*nv) * 3] = (i & 1) ? hi.x : lo.x;
        v[(*nv) * 3 + 1] = (i & 2) ? hi.y : lo.y;
        v[(*nv) * 3 + 2] = (i & 4) ? hi.z : lo.z;
        (*nv)++;
    }
    static const u32 q[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
    for (int f = 0; f < 6; ++f) {
        u32 t[6] = { q[f][0], q[f][1], q[f][2], q[f][0], q[f][2], q[f][3] };
        for (int k = 0; k < 6; ++k) tri[(*nt) * 3 + k] = b + t[k];
        *nt += 2;
    }
}

/* An anchored arch made as one mesh part (two pillars and a lintel): the
 * character walks through the gap, and a ball dropped on a pillar stays
 * on top of it. */
static void test_mesh_collision(void)
{
    ub_world w;
    door_world(&w, NULL);
    i32 ws = ub_world_workspace(&w);
    /* move the door walls away: the arch stands at z = 20 */
    f32 v[3 * 24];
    u32 tri[3 * 36], nv = 0, nt = 0, pv[3], pt[3];
    mesh_box(v, &nv, tri, &nt, v3(-0.5f, -0.5f, -0.5f), v3(-0.2f, 0.5f, 0.5f));
    pv[0] = nv; pt[0] = nt;
    mesh_box(v, &nv, tri, &nt, v3(0.2f, -0.5f, -0.5f), v3(0.5f, 0.5f, 0.5f));
    pv[1] = nv; pt[1] = nt;
    mesh_box(v, &nv, tri, &nt, v3(-0.2f, 0.3f, -0.5f), v3(0.2f, 0.5f, 0.5f));
    pv[2] = nv; pt[2] = nt;
    u16 mesh = ub_world_add_mesh(&w, v, nv, tri, nt, pv, pt, 3, 0);
    CHECK(mesh == 1);
    i32 arch = add_box(&w, ws, "Arch", v3(0, 5, 20), v3(12, 10, 2), 0x808080);
    w.parts[w.inst[arch].part].mesh = mesh;
    i32 ball = add_box(&w, ws, "Ball", v3(-4.2f, 14, 20), v3(1.5f, 1.5f, 1.5f), 0xFF0000);
    w.parts[w.inst[ball].part].flags &= (u8)~UB_PART_ANCHORED;
    w.parts[w.inst[ball].part].shape = UB_SHAPE_BALL;
    i32 door = ub_world_find_child(&w, ws, "Door");
    if (door >= 0) w.parts[w.inst[door].part].pos = v3(0, -50, 0); /* out of the way */
    CHECK(ub_game_start(&g_g, &w, false));
    ub_game_teleport(&g_g, v3(0, 0.1f, 14));
    f32 z = walk_watching(&g_g, 2.0f, NULL, -1);
    printf("  mesh part: walked through the arch to z = %.1f\n", (double)z);
    CHECK(z > 24);
    const ub_part *bp = &g_g.world.parts[g_g.world.inst[ball].part];
    printf("  mesh part: the ball rests at y = %.2f on the pillar (top 10)\n", (double)bp->pos.y);
    CHECK(bp->pos.y > 10.3f && bp->pos.y < 11.0f);
    /* the hull builder: a box piece is exact; a round one is fitted */
    ub_hull h;
    vec3 pts[8];
    for (int i = 0; i < 8; ++i) pts[i] = v3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
    CHECK(ub_hull_build(&h, pts, 8, tri, 12) && h.vert_count == 8 && h.face_count == 6 && h.edge_count == 12);
    CHECK(fabsf(h.volume - 0.3f) < 1e-4f);
    vec3 ballpts[200];
    int nb = 0;
    for (int a = 0; a < 10; ++a)
        for (int b = 0; b < 20; ++b) {
            f32 th = 3.14159f * ((f32)a + 0.5f) / 10.0f, ph = 6.28318f * (f32)b / 20.0f;
            ballpts[nb++] = v3(0.5f * sinf(th) * cosf(ph), 0.5f * cosf(th), 0.5f * sinf(th) * sinf(ph));
        }
    CHECK(ub_hull_build(&h, ballpts, nb, NULL, 0) && h.face_count <= UB_HULL_MAX_FACES && h.face_count >= 14);
    CHECK(h.volume > 0.45f && h.volume < 0.75f); /* a sphere of radius 0.5 is 0.52 */
    ub_game_stop(&g_g);
}

/* ------------------------------------------ the wider 2022 API (8B) */

/* A frame with the camera behind the spawn point looking at workspace.Button:
 * mouse in the middle; mouse button and keys as given (vk; 0 none). */
static void interact_frame(ub_game *g, bool press, bool release, int key_press, int key_hold, int key_release,
                           bool jump, f64 t)
{
    ub_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.backend = UB_BACKEND_SOFTWARE;
    fr.width = 800;
    fr.height = 636;
    static ub_input in;
    memset(&in, 0, sizeof in);
    in.has_focus = true;
    in.mouse_x = 400;
    in.mouse_y = 318;
    in.mouse_down[0] = press;
    in.mouse_pressed[0] = press;
    in.mouse_released[0] = release;
    if (key_press) in.key_pressed[key_press] = in.key_down[key_press] = true;
    if (key_hold) in.key_down[key_hold] = true;
    if (key_release) in.key_released[key_release] = true;
    ub_camera cam;
    memset(&cam, 0, sizeof cam);
    cam.pos = v3(0, 2, -6);
    cam.fov_y = 70.0f * 3.14159265f / 180.0f;
    cam.far_dist = 1000;
    play_gui_input(g, &fr, &in, &cam, false, t);
    ub_game_update(g, 1.0f / 60.0f, v3(0, 0, 0), jump);
    ub_gui_render(g->gui, &g->world, &cam);
}

static void test_api2022(void)
{
    char *src = read_text("tests/data/scripts/api2022.lua");
    CHECK(src != NULL);
    if (!src) return;
    ub_world w;
    door_world(&w, NULL);
    i32 ws = ub_world_workspace(&w);
    i32 button = add_box(&w, ws, "Button", v3(0, 2, 4), v3(2, 2, 1), 0xC03030);
    add_gui(&w, button, "ClickDetector", "ClickDetector");
    i32 chest = add_box(&w, ws, "Chest", v3(3, 1, 0), v3(2, 2, 2), 0x8B5A2B);
    add_gui(&w, chest, "ProximityPrompt", "ProximityPrompt");
    i32 crate = add_box(&w, ws, "Crate", v3(-3, 1, 0), v3(2, 2, 2), 0x8B5A2B);
    i32 held = add_gui(&w, crate, "ProximityPrompt", "ProximityPrompt");
    ub_value hd = ub_vfloat(0.5f), key = ub_venum(102); /* KeyCode.F */
    CHECK(ub_world_set_prop_named(&w, held, "HoldDuration", &hd) && ub_world_set_prop_named(&w, held, "KeyboardKeyCode", &key));
    i32 sss = ub_world_add(&w, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    add_script(&w, sss, "Api2022", src);
    free(src);
    CHECK(ub_game_start(&g_g, &w, true));
    int ready = -1;
    for (int i = 0; i < 900 && find_result(&g_g, "api2022 done") < 0; ++i) {
        if (ready < 0 && find_result(&g_g, "api2022 ready") >= 0) ready = i;
        int k = ready >= 0 ? i - ready : -1;
        interact_frame(&g_g, k == 5, k == 6, k == 15 ? 'E' : k == 25 ? 'F' : 0, k > 25 && k < 70 ? 'F' : 0,
                       k == 16 ? 'E' : k == 70 ? 'F' : 0, k == 90, i / 60.0);
    }
    CHECK(find_result(&g_g, "api2022 done") >= 0);
    bool ok = find_result(&g_g, "api2022 ok") >= 0;
    CHECK(ok);
    /* the prompts were drawn: a BillboardGui under CoreGui */
    i32 core = -1;
    for (i32 i = 0; i < g_g.world.count; ++i)
        if (g_g.world.inst[i].parent < 0 && ub_world_alive(&g_g.world, i) &&
            strcmp(ub_world_str(&g_g.world, g_g.world.inst[i].name), "CoreGui") == 0)
            core = i;
    CHECK(core >= 0 && g_g.world.inst[core].first_child >= 0);
    ub_script_stats st;
    ub_script_get_stats(&st);
    CHECK(st.errors == 0);
    if (!ok || st.errors) print_output();
    ub_game_stop(&g_g);
}

void test_game(void)
{
    test_door();
    test_api();
    test_luaulib();
    test_gui_api();
    test_api2022();
    test_mesh_collision();
    test_hostile();
    test_reuse();
    test_death();
    test_demo_place();
}
