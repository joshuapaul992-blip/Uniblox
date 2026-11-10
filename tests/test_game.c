/* Uniblox - whole games run headless: scripts, the character and physics
 * together. The M5 exit test is here: a touch-activated door works, and
 * hostile scripts are contained. */
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <time.h>

#include <math.h>
#include "game/ub_demo_place.h"
#include "game/ub_game.h"
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
    CHECK(st.scripts == 5);
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

void test_game(void)
{
    test_door();
    test_api();
    test_hostile();
    test_reuse();
    test_death();
    test_demo_place();
}
