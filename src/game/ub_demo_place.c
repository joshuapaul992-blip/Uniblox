/* Uniblox - the built-in demo place (also run by the tests). */
#include "game/ub_demo_place.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "render/ub_texture.h"
#include "render/ub_mesh.h"
#include "world/ub_classes.h"
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
    "\tlocal player = game:GetService('Players'):GetPlayerFromCharacter(hit.Parent)\n"
    "\tlocal stat = player and player:FindFirstChild('leaderstats') and player.leaderstats:FindFirstChild('Doors')\n"
    "\tif stat then stat.Value += 1 end\n"
    "\tlocal events = game:GetService('ReplicatedStorage'):FindFirstChild('DemoEvents')\n"
    "\tif events then events:FireAllClients('door') end\n"
    "\tdoor.CanCollide = false\n"
    "\tfor t = 0, 0.7, 0.1 do door.Transparency = t task.wait(0.03) end\n"
    "\ttask.wait(3)\n"
    "\tfor t = 0.7, 0, -0.1 do door.Transparency = t task.wait(0.03) end\n"
    "\tdoor.Transparency = 0\n"
    "\tdoor.CanCollide = true\n"
    "\topen = false\n"
    "end)\n";

static const char k_spin_script[] =
    "-- click the block (its ClickDetector) to turn it the other way\n"
    "local part = script.Parent\n"
    "local home = part.Position\n"
    "local t, way = 0, 1\n"
    "part.ClickDetector.MouseClick:Connect(function(player)\n"
    "\tway = -way\n"
    "\tpart.Color = if way > 0 then Color3.fromRGB(46, 139, 239) else Color3.fromRGB(239, 139, 46)\n"
    "end)\n"
    "game:GetService('RunService').Heartbeat:Connect(function(dt)\n"
    "\tt += dt * way\n"
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
    "\tprint('Welcome, ' .. player.Name .. '! Touch the door, press E at the lamp, click the spinning block, avoid the lava.')\n"
    "\tlocal leaderstats = Instance.new('Folder')\n"
    "\tleaderstats.Name = 'leaderstats'\n"
    "\tlocal doors = Instance.new('IntValue')\n"
    "\tdoors.Name = 'Doors'\n"
    "\tdoors.Parent = leaderstats\n"
    "\tleaderstats.Parent = player\n"
    "\tplayer.Chatted:Connect(function(message)\n"
    "\t\tlocal humanoid = player.Character and player.Character:FindFirstChildOfClass('Humanoid')\n"
    "\t\tif message == 'fast' and humanoid then humanoid.WalkSpeed = 32 print('Fast!')\n"
    "\t\telseif message == 'slow' and humanoid then humanoid.WalkSpeed = 16 print('Back to walking.')\n"
    "\t\telseif message == 'night' then game.Lighting.ClockTime = 0\n"
    "\t\telseif message == 'day' then game.Lighting.ClockTime = 14\n"
    "\t\tend\n"
    "\tend)\n"
    "end)\n";

/* the lamp by the path: its ProximityPrompt switches it (Luau 2022: types,
 * if-expressions, attributes, CollectionService tags) */
static const char k_lamp_script[] =
    "local CollectionService = game:GetService('CollectionService')\n"
    "local function wire(lamp: Model)\n"
    "\tlocal bulb = lamp:WaitForChild('Bulb') :: Part\n"
    "\tlocal prompt = bulb:WaitForChild('ProximityPrompt') :: ProximityPrompt\n"
    "\tlamp:SetAttribute('On', false)\n"
    "\tprompt.Triggered:Connect(function(player: Player)\n"
    "\t\tlocal on = not lamp:GetAttribute('On')\n"
    "\t\tlamp:SetAttribute('On', on)\n"
    "\t\tbulb.Material = if on then Enum.Material.Neon else Enum.Material.SmoothPlastic\n"
    "\t\tbulb.Color = if on then Color3.fromRGB(255, 214, 120) else Color3.fromRGB(150, 150, 150)\n"
    "\t\tprompt.ActionText = if on then 'Switch off' else 'Switch on'\n"
    "\tend)\n"
    "end\n"
    "for _, lamp in CollectionService:GetTagged('Lamp') do wire(lamp) end\n";

static void add_script(i32 parent, const char *name, const char *source)
{
    i32 s = ub_world_add(W, parent, UB_CLASS_OTHER, "Script", name);
    if (s >= 0) ub_world_set_source(W, s, source);
}

/* ------------------------------------------------------------- the GUI */

static const char k_hud_script[] =
    "-- the demo's on-screen panel: a LocalScript in a ScreenGui from StarterGui\n"
    "local events = game:GetService('ReplicatedStorage'):WaitForChild('DemoEvents')\n"
    "local gui = script.Parent\n"
    "local panel = gui:WaitForChild('Panel')\n"
    "local stats = panel:WaitForChild('Stats')\n"
    "local toast = gui:WaitForChild('Toast')\n"
    "local doors = 0\n"
    "local shown = 0\n"
    "local function show(message)\n"
    "\tshown += 1\n"
    "\tlocal mine = shown\n"
    "\ttoast.Text = message\n"
    "\ttoast.Visible = true\n"
    "\ttoast.Position = UDim2.new(0.5, 0, 1, 60)\n"
    "\ttoast:TweenPosition(UDim2.new(0.5, 0, 1, -24), Enum.EasingDirection.Out, Enum.EasingStyle.Quad, 0.3, true)\n"
    "\ttask.delay(2.5, function()\n"
    "\t\tif shown == mine then toast.Visible = false end\n"
    "\tend)\n"
    "end\n"
    "events.OnClientEvent:Connect(function(what)\n"
    "\tif what == 'door' then\n"
    "\t\tdoors += 1\n"
    "\t\tstats.Text = 'Doors opened: ' .. doors\n"
    "\t\tshow('The door opened')\n"
    "\tend\n"
    "end)\n"
    "panel:WaitForChild('Home').Activated:Connect(function()\n"
    "\tevents:FireServer('home')\n"
    "\tshow('Back to the start')\n"
    "end)\n"
    "show('Welcome to Uniblox')\n"
    "game:GetService('StarterGui'):SetCore('SendNotification', {Title = 'Uniblox demo', Text = 'Press / to chat, Tab for the player list, Esc for the menu.', Duration = 8})\n"
    "print('The demo panel is ready')\n";

static const char k_server_script[] =
    "-- answers the demo panel\n"
    "local events = game:GetService('ReplicatedStorage'):WaitForChild('DemoEvents')\n"
    "events.OnServerEvent:Connect(function(player, what)\n"
    "\tlocal root = player.Character and player.Character:FindFirstChild('HumanoidRootPart')\n"
    "\tlocal spawn = workspace:FindFirstChild('SpawnLocation')\n"
    "\tif what == 'home' and root and spawn then\n"
    "\t\troot.CFrame = spawn.CFrame + Vector3.new(0, 3, 0)\n"
    "\t\tprint(player.Name .. ' went back to the start')\n"
    "\tend\n"
    "end)\n";

static i32 gui(i32 parent, const char *cls, const char *name)
{
    i32 i = ub_world_add(W, parent, UB_CLASS_OTHER, cls, name);
    if (i >= 0) ub_cdb_fill_defaults(W, i, cls);
    return i;
}

static void p_ud2(i32 i, const char *k, f32 sx, i32 ox, f32 sy, i32 oy)
{
    ub_value v;
    v.type = UB_PV_UDIM2;
    v.v.ud2.sx = sx;
    v.v.ud2.ox = ox;
    v.v.ud2.sy = sy;
    v.v.ud2.oy = oy;
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_ud(i32 i, const char *k, f32 sc, i32 o)
{
    ub_value v;
    v.type = UB_PV_UDIM;
    v.v.ud.s = sc;
    v.v.ud.o = o;
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_rgb(i32 i, const char *k, u32 rgb)
{
    ub_value v;
    v.type = UB_PV_COLOR3;
    v.v.c3.r = (f32)((rgb >> 16) & 255) / 255.0f;
    v.v.c3.g = (f32)((rgb >> 8) & 255) / 255.0f;
    v.v.c3.b = (f32)(rgb & 255) / 255.0f;
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_num(i32 i, const char *k, f32 f)
{
    ub_value v = ub_vfloat(f);
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_int(i32 i, const char *k, i32 n)
{
    ub_value v = ub_vint(n);
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_enum(i32 i, const char *k, u32 e)
{
    ub_value v = ub_venum(e);
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_bool(i32 i, const char *k, bool b)
{
    ub_value v = ub_vbool(b);
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_str(i32 i, const char *k, const char *s)
{
    ub_value v = ub_vstr(s, (u32)ub_strlen(s));
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_v2(i32 i, const char *k, f32 x, f32 y)
{
    ub_value v;
    v.type = UB_PV_VECTOR2;
    v.v.v2.x = x;
    v.v.v2.y = y;
    ub_world_set_prop_named(W, i, k, &v);
}

static void p_v3(i32 i, const char *k, f32 x, f32 y, f32 z)
{
    ub_value v;
    v.type = UB_PV_VECTOR3;
    v.v.v3.x = x;
    v.v.v3.y = y;
    v.v.v3.z = z;
    ub_world_set_prop_named(W, i, k, &v);
}

/* A label in Uniblox's own look: white text on nothing. */
static i32 label(i32 parent, const char *name, const char *text, f32 size, u32 color, int font)
{
    i32 l = gui(parent, "TextLabel", name);
    p_str(l, "Text", text);
    p_num(l, "TextSize", size);
    p_rgb(l, "TextColor3", color);
    p_num(l, "BackgroundTransparency", 1);
    p_enum(l, "Font", (u32)font);
    p_enum(l, "TextXAlignment", 0); /* Left */
    return l;
}

static void rounded(i32 parent, i32 radius)
{
    i32 c = gui(parent, "UICorner", "Corner");
    p_ud(c, "CornerRadius", 0, radius);
}

/* The panel in the corner, the message line, a sign by the spawn and a
 * label over the door. */
static void add_gui(i32 ws, i32 door)
{
    i32 rs = ub_world_add(W, -1, UB_CLASS_FOLDER, "ReplicatedStorage", "ReplicatedStorage");
    gui(rs, "RemoteEvent", "DemoEvents");
    i32 sg = gui(-1, "StarterGui", "StarterGui");
    W->inst[sg].cls = UB_CLASS_FOLDER;
    i32 hud = gui(sg, "ScreenGui", "DemoHud");
    p_bool(hud, "ResetOnSpawn", false);
    p_enum(hud, "ZIndexBehavior", 1); /* Sibling */

    i32 panel = gui(hud, "Frame", "Panel");
    p_v2(panel, "AnchorPoint", 1, 0);
    p_ud2(panel, "Position", 1, -12, 0, 64); /* top right, under the player list */
    p_ud2(panel, "Size", 0, 210, 0, 0);
    p_enum(panel, "AutomaticSize", 2); /* Y */
    p_rgb(panel, "BackgroundColor3", 0x1C2430);
    p_num(panel, "BackgroundTransparency", 0.2f);
    rounded(panel, 10);
    i32 stroke = gui(panel, "UIStroke", "Edge");
    p_rgb(stroke, "Color", 0xFFFFFF);
    p_num(stroke, "Transparency", 0.75f);
    i32 pad = gui(panel, "UIPadding", "Padding");
    p_ud(pad, "PaddingTop", 0, 10);
    p_ud(pad, "PaddingBottom", 0, 10);
    p_ud(pad, "PaddingLeft", 0, 12);
    p_ud(pad, "PaddingRight", 0, 12);
    i32 list = gui(panel, "UIListLayout", "List");
    p_ud(list, "Padding", 0, 6);
    p_enum(list, "SortOrder", 2); /* LayoutOrder */
    i32 title = label(panel, "Title", "Uniblox demo", 20, 0xFFFFFF, 4 /* SourceSansBold */);
    p_ud2(title, "Size", 1, 0, 0, 22);
    p_int(title, "LayoutOrder", 1);
    i32 stats = label(panel, "Stats", "Doors opened: 0", 15, 0xC8D2DC, 3 /* SourceSans */);
    p_ud2(stats, "Size", 1, 0, 0, 18);
    p_int(stats, "LayoutOrder", 2);
    i32 home = gui(panel, "TextButton", "Home");
    p_str(home, "Text", "Back to the start");
    p_num(home, "TextSize", 15);
    p_enum(home, "Font", 4);
    p_rgb(home, "TextColor3", 0xFFFFFF);
    p_rgb(home, "BackgroundColor3", 0x2F7BE0);
    p_ud2(home, "Size", 1, 0, 0, 30);
    p_int(home, "LayoutOrder", 3);
    p_int(home, "BorderSizePixel", 0);
    rounded(home, 6);
    i32 grad = gui(home, "UIGradient", "Shine");
    {
        f32 keys[8] = { 0, 1, 1, 1, 1, 0.78f, 0.86f, 1 };
        ub_value v;
        v.type = UB_PV_COLORSEQ;
        v.v.seq.k = keys;
        v.v.seq.n = 2;
        ub_world_set_prop_named(W, grad, "Color", &v);
        p_num(grad, "Rotation", 90);
    }

    i32 toast = label(hud, "Toast", "", 18, 0xFFFFFF, 4);
    p_v2(toast, "AnchorPoint", 0.5f, 1);
    p_ud2(toast, "Position", 0.5f, 0, 1, -24);
    p_ud2(toast, "Size", 0, 260, 0, 36);
    p_enum(toast, "TextXAlignment", 2); /* Center */
    p_rgb(toast, "BackgroundColor3", 0x1C2430);
    p_num(toast, "BackgroundTransparency", 0.25f);
    p_bool(toast, "Visible", false);
    rounded(toast, 18);
    i32 ls = ub_world_add(W, hud, UB_CLASS_OTHER, "LocalScript", "HudScript");
    if (ls >= 0) {
        ub_world_set_source(W, ls, k_hud_script);
        W->inst[ls].flags |= UB_INST_CLIENT;
    }

    /* a sign by the spawn: a SurfaceGui on the side facing the player */
    i32 sign = add_part(ws, "Sign", UB_SHAPE_BOX, -7, 3.2f, 8, 6, 3, 0.3f, 0xE8E2D4, UB_MAT_WOOD);
    add_part(ws, "SignPost", UB_SHAPE_BOX, -7, 0.9f, 8, 0.3f, 1.8f, 0.3f, 0x6E4A26, UB_MAT_WOOD);
    i32 sgui = gui(sign, "SurfaceGui", "Board");
    p_enum(sgui, "Face", 2);       /* Back: Roblox's +Z, towards the spawn */
    p_enum(sgui, "SizingMode", 1); /* PixelsPerStud */
    p_num(sgui, "PixelsPerStud", 50);
    i32 big = label(sgui, "Heading", "Welcome to Uniblox", 34, 0x2B3A4A, 4);
    p_ud2(big, "Size", 1, 0, 0.5f, 0);
    p_ud2(big, "Position", 0, 0, 0.12f, 0);
    p_enum(big, "TextXAlignment", 2);
    i32 small = label(sgui, "Hint", "Walk to the door. Mind the lava.", 20, 0x5A6470, 3);
    p_ud2(small, "Size", 1, 0, 0.3f, 0);
    p_ud2(small, "Position", 0, 0, 0.58f, 0);
    p_enum(small, "TextXAlignment", 2);

    /* over the door: a BillboardGui */
    i32 bb = gui(door, "BillboardGui", "Label");
    p_ud2(bb, "Size", 0, 150, 0, 36);
    /* in world axes (StudsOffset is the camera's): up, and out towards the spawn (Roblox's +Z) */
    p_v3(bb, "StudsOffsetWorldSpace", 0, 5.5f, 1.5f);
    p_num(bb, "MaxDistance", 90);
    i32 tag = label(bb, "Text", "Touch to open", 18, 0xFFFFFF, 4);
    p_ud2(tag, "Size", 1, 0, 1, 0);
    p_enum(tag, "TextXAlignment", 2);
    p_rgb(tag, "BackgroundColor3", 0x1C2430);
    p_num(tag, "BackgroundTransparency", 0.3f);
    rounded(tag, 18);
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
    i32 door = add_part(house, "Door", UB_SHAPE_BOX, 0, 4, 21.5f, 8, 8, 0.8f, 0x8B5A2B, UB_MAT_WOOD);
    add_script(door, "DoorScript", k_door_script);
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
    i32 spinner = add_part(house, "Spinner", UB_SHAPE_BOX, 0, 6, 40, 3, 3, 3, 0x2E8BEF, UB_MAT_PLASTIC);
    gui(spinner, "ClickDetector", "ClickDetector");
    add_script(spinner, "Spin", k_spin_script);
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
    /* a lamp beside the path, tagged "Lamp", with a prompt (press E) */
    i32 lamp = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "Lamp");
    add_part(lamp, "Post", UB_SHAPE_CYLINDER, -5, 3, 9, 0.6f, 6, 0.6f, 0x3A3F47, UB_MAT_METAL);
    i32 bulb = add_part(lamp, "Bulb", UB_SHAPE_BALL, -5, 6.6f, 9, 1.4f, 1.4f, 1.4f, 0x969696, UB_MAT_PLASTIC);
    i32 prompt = gui(bulb, "ProximityPrompt", "ProximityPrompt");
    p_str(prompt, "ActionText", "Switch on");
    p_str(prompt, "ObjectText", "Lamp");
    ub_value tag = ub_vstr("Lamp", 4);
    ub_world_set_prop_named(W, lamp, "Tags", &tag);
    /* a ball on a rope under a beam (a RopeConstraint between two attachments): push it */
    {
        i32 swing = ub_world_add(W, ws, UB_CLASS_MODEL, NULL, "RopeSwing");
        add_part(swing, "PostA", UB_SHAPE_BOX, -26, 6, 30, 1, 12, 1, 0x6E4A26, UB_MAT_WOOD);
        add_part(swing, "PostB", UB_SHAPE_BOX, -18, 6, 30, 1, 12, 1, 0x6E4A26, UB_MAT_WOOD);
        i32 beam = add_part(swing, "Beam", UB_SHAPE_BOX, -22, 12.5f, 30, 9, 1, 1, 0x6E4A26, UB_MAT_WOOD);
        i32 ball = loosen(add_part(swing, "Ball", UB_SHAPE_BALL, -22, 5, 30, 2.5f, 2.5f, 2.5f, 0xD94040, UB_MAT_PLASTIC));
        i32 a0 = gui(beam, "Attachment", "Top"), a1 = gui(ball, "Attachment", "Knot");
        ub_value cf;
        memset(&cf, 0, sizeof cf);
        f32 m[12] = { 0, -0.5f, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        cf.type = UB_PV_CFRAME;
        cf.v.cf.m = m;
        ub_world_set_prop_named(W, a0, "CFrame", &cf);
        m[1] = 1.25f;
        ub_world_set_prop_named(W, a1, "CFrame", &cf);
        i32 rope = gui(beam, "RopeConstraint", "Rope");
        ub_value r0, r1, len = ub_vfloat(7.0f), vis = ub_vbool(true);
        memset(&r0, 0, sizeof r0);
        r0.type = UB_PV_REF;
        r0.v.ref = a0;
        r1 = r0;
        r1.v.ref = a1;
        ub_world_set_prop_named(W, rope, "Attachment0", &r0);
        ub_world_set_prop_named(W, rope, "Attachment1", &r1);
        ub_world_set_prop_named(W, rope, "Length", &len);
        ub_world_set_prop_named(W, rope, "Visible", &vis);
    }
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
    add_script(sss, "PanelServer", k_server_script);
    add_script(sss, "Lamps", k_lamp_script);
    add_gui(ws, door);
}
