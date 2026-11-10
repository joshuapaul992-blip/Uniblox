/* Uniblox - the built-in demo place (also run by the tests). */
#include "game/ub_demo_place.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "render/ub_texture.h"
#include "render/ub_mesh.h"
#include "crt/ub_crt.h"

static ub_world *W;

static i32 add_part(i32 parent, const char *name, ub_shape shape, f32 x, f32 y, f32 z, f32 sx, f32 sy,
                    f32 sz, u32 color, ub_material mat)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = v3(x, y, z);
    p.rot = mat3_identity();
    p.size = v3(sx, sy, sz);
    p.color = color;
    p.shape = (u8)shape;
    p.material = (u8)mat;
    p.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    return ub_world_add_part(W, parent, UB_CLASS_PART, NULL, name, &p);
}

/* Make a part loose (unanchored), so physics moves it. */
static i32 loosen(i32 inst)
{
    if (inst >= 0) W->parts[W->inst[inst].part].flags &= (u8)~UB_PART_ANCHORED;
    return inst;
}

static void add_tree(i32 ws, f32 x, f32 z, f32 scale)
{
    i32 tree = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "Tree");
    add_part(tree, "Trunk", UB_SHAPE_CYLINDER, x, 3.0f * scale, z, 1.2f * scale, 6.0f * scale, 1.2f * scale,
             0x6B4423, UB_MAT_WOOD);
    add_part(tree, "Leaves", UB_SHAPE_BALL, x, 7.5f * scale, z, 6.0f * scale, 5.0f * scale, 6.0f * scale,
             0x3A9A3A, UB_MAT_GRASS);
}

/* The demo place's scripts, Roblox-style (positions in Roblox's axes). */
static const char k_door_script[] =
    "-- touch the door to open it; it shuts again after a few seconds\n"
    "local door = script.Parent\n"
    "local open = false\n"
    "door.Touched:Connect(function(hit)\n"
    "\tif open or not hit.Parent:FindFirstChildOfClass('Humanoid') then return end\n"
    "\topen = true\n"
    "\tprint(hit.Parent.Name .. ' opened the door')\n"
    "\tdoor.CanCollide = false\n"
    "\tfor t = 0, 0.7, 0.1 do door.Transparency = t task.wait(0.03) end\n"
    "\ttask.wait(3)\n"
    "\tfor t = 0.7, 0, -0.1 do door.Transparency = t task.wait(0.03) end\n"
    "\tdoor.Transparency = 0\n"
    "\tdoor.CanCollide = true\n"
    "\topen = false\n"
    "end)\n";

static const char k_spin_script[] =
    "local part = script.Parent\n"
    "local home = part.Position\n"
    "local t = 0\n"
    "game:GetService('RunService').Heartbeat:Connect(function(dt)\n"
    "\tt += dt\n"
    "\tpart.CFrame = CFrame.new(home + Vector3.new(0, math.sin(t * 2) * 0.5, 0))\n"
    "\t\t* CFrame.Angles(t * 0.9, t * 0.5, t * 0.3)\n"
    "end)\n";

static const char k_lava_script[] =
    "local lava = script.Parent\n"
    "lava.Touched:Connect(function(hit)\n"
    "\tlocal humanoid = hit.Parent:FindFirstChildOfClass('Humanoid')\n"
    "\tif humanoid and humanoid.Health > 0 then\n"
    "\t\thumanoid.Health = 0\n"
    "\t\tprint(hit.Parent.Name .. ' fell in the lava')\n"
    "\tend\n"
    "end)\n"
    "-- glow brighter and dimmer, for ever\n"
    "local info = TweenInfo.new(1.2, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)\n"
    "game:GetService('TweenService'):Create(lava, info, { Color = Color3.fromRGB(255, 180, 40) }):Play()\n";

static const char k_button_script[] =
    "-- step on the button: balls rain down\n"
    "local button = script.Parent\n"
    "local Debris = game:GetService('Debris')\n"
    "local busy = false\n"
    "button.Touched:Connect(function(hit)\n"
    "\tif busy or not hit.Parent:FindFirstChildOfClass('Humanoid') then return end\n"
    "\tbusy = true\n"
    "\tbutton.BrickColor = BrickColor.new('Bright red')\n"
    "\tfor i = 1, 6 do\n"
    "\t\tlocal ball = Instance.new('Part')\n"
    "\t\tball.Shape = Enum.PartType.Ball\n"
    "\t\tball.Size = Vector3.new(2, 2, 2)\n"
    "\t\tball.BrickColor = BrickColor.random()\n"
    "\t\tball.Position = button.Position + Vector3.new(math.random(-6, 6), 20 + i * 3, math.random(-6, 6))\n"
    "\t\tball.Parent = workspace\n"
    "\t\tDebris:AddItem(ball, 15)\n"
    "\tend\n"
    "\ttask.wait(1)\n"
    "\tbutton.BrickColor = BrickColor.new('Bright green')\n"
    "\tbusy = false\n"
    "end)\n";

static const char k_welcome_script[] =
    "local Players = game:GetService('Players')\n"
    "Players.PlayerAdded:Connect(function(player)\n"
    "\tprint('Welcome, ' .. player.Name .. '! Touch the door, step on the green button, avoid the lava.')\n"
    "\tplayer.Chatted:Connect(function(message)\n"
    "\t\tlocal humanoid = player.Character and player.Character:FindFirstChildOfClass('Humanoid')\n"
    "\t\tif message == 'fast' and humanoid then humanoid.WalkSpeed = 32 print('Fast!')\n"
    "\t\telseif message == 'slow' and humanoid then humanoid.WalkSpeed = 16 print('Back to walking.')\n"
    "\t\telseif message == 'night' then game.Lighting.ClockTime = 0\n"
    "\t\telseif message == 'day' then game.Lighting.ClockTime = 14\n"
    "\t\tend\n"
    "\tend)\n"
    "end)\n";

static void add_script(i32 parent, const char *name, const char *source)
{
    i32 s = ub_world_add(W, parent, UB_CLASS_OTHER, "Script", name);
    if (s >= 0) ub_world_set_source(W, s, source);
}

/* The built-in demo place: a walled room (open roof) with a door facing
 * the spawn point, and some things outside. Room interior: x -18..18,
 * z 22..58. */
void ub_demo_place(ub_world *w)
{
    W = w;
    ub_world_init(W);
    ub_fmt(W->name, sizeof W->name, "Uniblox demo");
    i32 ws = ub_world_workspace(W);
    ub_world_add(W, -1, UB_CLASS_LIGHTING, NULL, "Lighting");
    W->lighting.clock_time = 14.0f;
    W->lighting.fog_end = 850.0f;

    add_part(ws, "Baseplate", UB_SHAPE_BOX, 0, -0.5f, 20, 300, 1, 300, 0x4C8C3C, UB_MAT_GRASS);
    i32 house = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "House");
    add_part(house, "Floor", UB_SHAPE_BOX, 0, 0.05f, 40, 36, 0.1f, 36, 0xA0703C, UB_MAT_WOOD);
    add_part(house, "NorthWall", UB_SHAPE_BOX, 0, 6, 58.5f, 38, 12, 1, 0xB04A30, UB_MAT_BRICK);
    add_part(house, "WestWall", UB_SHAPE_BOX, -18.5f, 6, 40, 1, 12, 36, 0xB04A30, UB_MAT_BRICK);
    add_part(house, "EastWall", UB_SHAPE_BOX, 18.5f, 6, 40, 1, 12, 36, 0xB04A30, UB_MAT_BRICK);
    add_part(house, "SouthWallLeft", UB_SHAPE_BOX, -11.5f, 6, 21.5f, 15, 12, 1, 0xB04A30, UB_MAT_BRICK);
    add_script(add_part(house, "Door", UB_SHAPE_BOX, 0, 4, 21.5f, 8, 8, 0.8f, 0x8B5A2B, UB_MAT_WOOD), "DoorScript",
               k_door_script);
    add_part(house, "SouthWallRight", UB_SHAPE_BOX, 11.5f, 6, 21.5f, 15, 12, 1, 0xB04A30, UB_MAT_BRICK);
    add_part(house, "Lintel", UB_SHAPE_BOX, 0, 10, 21.5f, 8, 4, 1, 0xB04A30, UB_MAT_BRICK);
    for (int i = 0; i < 4; ++i)
        add_part(house, "Pillar", UB_SHAPE_CYLINDER, (i & 1) ? 14.0f : -14.0f, 6, (i & 2) ? 54.0f : 26.0f,
                 2.5f, 12, 2.5f, 0x9A9A9A, UB_MAT_STONE);
    for (int i = 0; i < 6; ++i)
        add_part(house, "Step", UB_SHAPE_BOX, 15, (f32)(i + 1) * 0.5f, 31.0f + (f32)i * 3.0f, 5, (f32)(i + 1), 3,
                 0x8B5A2B, UB_MAT_WOOD);
    add_part(house, "Ramp", UB_SHAPE_WEDGE, -12, 1.5f, 46, 6, 3, 8, 0x8090A0, UB_MAT_METAL);
    add_part(house, "Landing", UB_SHAPE_BOX, -12, 1.5f, 53, 6, 3, 6, 0x8090A0, UB_MAT_METAL);
    add_part(house, "Pedestal", UB_SHAPE_CYLINDER, 0, 1.5f, 40, 4, 3, 4, 0x7A8590, UB_MAT_METAL);
    add_script(add_part(house, "Spinner", UB_SHAPE_BOX, 0, 6, 40, 3, 3, 3, 0x2E8BEF, UB_MAT_PLASTIC), "Spin",
               k_spin_script);
    loosen(add_part(house, "Ball", UB_SHAPE_BALL, -6, 1.6f, 30, 3, 3, 3, 0xF2C230, UB_MAT_PLASTIC));
    loosen(add_part(house, "Ball", UB_SHAPE_BALL, 6, 1.1f, 33, 2, 2, 2, 0x30C060, UB_MAT_PLASTIC));
    loosen(add_part(house, "Ball", UB_SHAPE_BALL, -4, 1.1f, 50, 2, 2, 2, 0xE04080, UB_MAT_PLASTIC));
    i32 table = ub_world_add(W, house, UB_CLASS_MODEL, NULL, "Table");
    add_part(table, "Top", UB_SHAPE_BOX, 6, 3.0f, 50, 6, 0.4f, 4, 0x9C6B3A, UB_MAT_WOOD);
    for (int i = 0; i < 4; ++i)
        add_part(table, "Leg", UB_SHAPE_BOX, (i & 1) ? 8.6f : 3.4f, 1.4f, (i & 2) ? 51.6f : 48.4f,
                 0.4f, 2.8f, 0.4f, 0x6E4A26, UB_MAT_WOOD);

    ub_part spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.pos = v3(0, 0.05f, 2.5f);
    spawn.rot = mat3_identity();
    spawn.size = v3(4, 0.1f, 4);
    spawn.color = 0x8A8A8A;
    spawn.shape = UB_SHAPE_BOX;
    spawn.material = UB_MAT_STONE;
    spawn.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    ub_world_add_part(W, ws, UB_CLASS_SPAWN, NULL, "SpawnLocation", &spawn);
    for (int i = 1; i < 4; ++i)
        add_part(ws, "Path", UB_SHAPE_BOX, 0, 0.05f, 2.5f + (f32)i * 5.0f, 4, 0.1f, 4, 0x8A8A8A, UB_MAT_STONE);
    add_tree(ws, -30, 8, 1.0f);
    add_tree(ws, 32, -6, 1.2f);
    add_tree(ws, -34, 68, 0.9f);
    add_tree(ws, 38, 62, 1.1f);
    add_tree(ws, -8, -30, 1.0f);
    /* loose blocks: a twisted tower and a pyramid of crates (they settle,
     * then sleep; push them over) */
    static const u32 tower[4] = { 0xD94040, 0xF2B233, 0x3FAE4A, 0x3F6FD9 };
    i32 stack = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "Tower");
    for (int i = 0; i < 4; ++i) {
        i32 b = add_part(stack, "Block", UB_SHAPE_BOX, 28, 1.5f + (f32)i * 3.02f, 20, 3, 3, 3, tower[i], UB_MAT_PLASTIC);
        W->parts[W->inst[b].part].rot = mat3_from_euler((f32)i * 0.3f, 0, 0);
        loosen(b);
    }
    i32 crates = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "Crates");
    for (int row = 0; row < 3; ++row)
        for (int i = 0; i < 3 - row; ++i)
            loosen(add_part(crates, "Crate", UB_SHAPE_BOX, -15.0f + (f32)i * 3.1f + (f32)row * 1.55f,
                            1.5f + (f32)row * 3.02f, 13, 3, 3, 3, 0xB0884E, UB_MAT_WOOD));
    /* a cart: a plank and two wheels welded into one body */
    i32 cart = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "Cart");
    i32 deck = loosen(add_part(cart, "Deck", UB_SHAPE_BOX, 12, 1.6f, 8, 3, 0.4f, 5, 0x9C6B3A, UB_MAT_WOOD));
    for (int i = 0; i < 2; ++i) {
        i32 axle = loosen(add_part(cart, "Wheels", UB_SHAPE_CYLINDER, 12, 0.9f, i ? 9.8f : 6.2f, 1.6f, 3.6f, 1.6f,
                                   0x303030, UB_MAT_METAL));
        W->parts[W->inst[axle].part].rot = mat3_from_euler(0, 0, UB_HALF_PI); /* axle along x */
        ub_world_add_weld(W, deck, axle);
    }
    add_part(ws, "GridPad", UB_SHAPE_BOX, -40, 0.5f, -20, 20, 1, 20, 0xA3A2A5, UB_MAT_GRID);
    add_part(ws, "GridRamp", UB_SHAPE_WEDGE, -40, 1.5f, -7, 8, 2, 6, 0xA3A2A5, UB_MAT_GRID);
    i32 lava = add_part(ws, "Lava", UB_SHAPE_BOX, -40, 1.1f, -25, 8, 0.2f, 4, 0xFF4A1C, UB_MAT_PLASTIC);
    W->parts[W->inst[lava].part].flags |= UB_PART_GLOW;
    add_script(lava, "Burn", k_lava_script);
    add_script(add_part(ws, "BallButton", UB_SHAPE_CYLINDER, 18, 0.15f, -2, 4, 0.3f, 4, 0x3FAE4A, UB_MAT_METAL),
               "Balls", k_button_script);
    i32 sss = ub_world_add(W, -1, UB_CLASS_FOLDER, "ServerScriptService", "ServerScriptService");
    add_script(sss, "Welcome", k_welcome_script);
}
