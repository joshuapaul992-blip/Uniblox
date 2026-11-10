-- Uniblox API test: run as a Script in ServerScriptService by
-- tests/test_game.c. Makes workspace."api ok" when every check passes and
-- workspace."api done" when it finishes either way.

local failures = 0
local function check(cond, what)
	if not cond then
		failures += 1
		warn("FAIL: " .. what)
	end
end
local function near(a, b, eps)
	return math.abs(a - b) < (eps or 1e-4)
end
local function vnear(a, b, eps)
	return (a - b).Magnitude < (eps or 1e-3)
end

local ok, err = pcall(function()
	-- globals and services
	check(game == Game and workspace == Workspace and game.Workspace == workspace, "game/workspace")
	check(game:GetService("Workspace") == workspace, "GetService Workspace")
	local Players = game:GetService("Players")
	local RunService = game:GetService("RunService")
	local Lighting = game:GetService("Lighting")
	local RS = game:GetService("ReplicatedStorage")
	check(game.Players == Players and game.Lighting == Lighting, "game.<Service>")
	check(RunService:IsServer() and not RunService:IsStudio(), "RunService")
	check(not pcall(function() game:GetService("NoSuchService") end), "bad service")
	check(script.Name == "Api" and script.ClassName == "Script", "script")
	check(script.Parent.Name == "ServerScriptService", "script.Parent")
	check(typeof(workspace) == "Instance" and typeof(Vector3.new()) == "Vector3", "typeof")
	check(typeof(CFrame.new()) == "CFrame" and typeof(Color3.new()) == "Color3", "typeof 2")
	check(typeof(Enum.Material.Wood) == "EnumItem" and typeof(BrickColor.new("Bright red")) == "BrickColor", "typeof 3")

	-- the tree
	local stuff = workspace:FindFirstChild("Stuff")
	check(stuff and stuff:IsA("Model") and stuff:IsA("PVInstance") and stuff:IsA("Instance"), "Model")
	check(workspace.Stuff == stuff, "child by name")
	check(#stuff:GetChildren() == 2, "GetChildren")
	local a = stuff.A
	check(a:IsA("BasePart") and a:IsA("Part") and not a:IsA("Model"), "IsA part")
	check(a:IsDescendantOf(workspace) and workspace:IsAncestorOf(a), "descendant")
	check(a:GetFullName() == "Workspace.Stuff.A", "GetFullName " .. a:GetFullName())
	check(workspace:FindFirstChild("A") == nil and workspace:FindFirstChild("A", true) == a, "recursive find")
	check(stuff:FindFirstChildOfClass("Part") ~= nil and stuff:FindFirstChildWhichIsA("BasePart") ~= nil, "OfClass")
	check(a:FindFirstAncestorOfClass("Workspace") == workspace and a:FindFirstAncestor("Stuff") == stuff, "ancestors")
	local n = 0
	for _, d in ipairs(workspace:GetDescendants()) do
		if d:IsA("BasePart") then n += 1 end
	end
	check(n >= 8, "GetDescendants " .. n)
	check(not pcall(function() return a.NoSuchProperty end), "bad member errors")
	check(tostring(a) == "A", "tostring")

	-- parts: Roblox space (z is the other way round from Uniblox's)
	check(vnear(a.Position, Vector3.new(20, 1, 5)), "Position " .. tostring(a.Position))
	check(vnear(a.Size, Vector3.new(2, 2, 2)), "Size")
	check(a.Anchored == true and a.CanCollide == true, "flags")
	check(a.Color == Color3.new(1, 0, 0) or vnear(Vector3.new(a.Color.R, a.Color.G, a.Color.B), Vector3.new(1, 0, 0)), "Color")
	a.Position = Vector3.new(20, 3, 5)
	check(vnear(a.Position, Vector3.new(20, 3, 5)), "set Position")
	a.CFrame = CFrame.new(1, 2, 3) * CFrame.Angles(0, math.rad(90), 0)
	check(vnear(a.Position, Vector3.new(1, 2, 3)), "CFrame position")
	check(vnear(a.CFrame.LookVector, Vector3.new(-1, 0, 0)), "LookVector " .. tostring(a.CFrame.LookVector))
	check(vnear(a.Orientation, Vector3.new(0, 90, 0), 0.01), "Orientation " .. tostring(a.Orientation))
	a.Orientation = Vector3.new(0, 45, 0)
	check(vnear(a.Orientation, Vector3.new(0, 45, 0), 0.01), "set Orientation")
	a.Size = Vector3.new(4, 1, 2)
	check(vnear(a.Size, Vector3.new(4, 1, 2)), "set Size")
	a.Transparency = 0.5
	check(near(a.Transparency, 0.5, 0.01), "Transparency")
	a.Material = Enum.Material.Neon
	check(a.Material == Enum.Material.Neon, "Material")
	a.Material = "Wood"
	check(a.Material == Enum.Material.Wood and a.Material.Name == "Wood", "Material by name")
	a.BrickColor = BrickColor.new("Bright blue")
	check(a.BrickColor.Name == "Bright blue", "BrickColor " .. a.BrickColor.Name)
	a.Color = Color3.fromRGB(10, 20, 30)
	local c = a.Color
	check(near(c.R * 255, 10, 0.6) and near(c.G * 255, 20, 0.6) and near(c.B * 255, 30, 0.6), "Color3.fromRGB")
	a.CanCollide = false
	check(a.CanCollide == false, "CanCollide")

	-- the extra properties and attributes
	a.SomethingNew = 5
	check(a.SomethingNew == 5, "kept property")
	a:SetAttribute("Score", 10)
	check(a:GetAttribute("Score") == 10 and a:GetAttribute("Nope") == nil, "attributes")
	check(a:GetAttributes().Score == 10, "GetAttributes")

	-- Instance.new, parenting, Clone, Destroy, events
	local added, removed = 0, 0
	local c1 = workspace.ChildAdded:Connect(function(ch) added += 1 end)
	workspace.ChildRemoved:Connect(function(ch) removed += 1 end)
	local p = Instance.new("Part")
	check(p.Parent == nil and p.ClassName == "Part", "new part")
	check(vnear(p.Size, Vector3.new(4, 1, 2)), "default size")
	p.Name = "Made"
	p.Anchored = true
	p.Position = Vector3.new(0, 10, 0)
	p.Parent = workspace
	check(workspace.Made == p and added == 1, "parented " .. added)
	local q = p:Clone()
	check(q.Parent == nil and q.Name == "Made" and q ~= p, "Clone")
	q.Parent = workspace
	check(added == 2, "clone added")
	q:Destroy()
	check(q.Parent == nil and removed == 1, "Destroy")
	check(not pcall(function() q.Parent = workspace end), "destroyed is locked")
	c1:Disconnect()
	check(not c1.Connected, "Disconnect")
	Instance.new("Folder", workspace)
	check(added == 2, "disconnected handler")
	local folder = Instance.new("Folder")
	folder.Parent = workspace
	Instance.new("Part", folder)
	Instance.new("Part", folder)
	check(#folder:GetChildren() == 2, "children")
	folder:ClearAllChildren()
	check(#folder:GetChildren() == 0, "ClearAllChildren")
	check(not pcall(function() folder.Parent = folder end), "circular parent")
	check(not pcall(function() workspace.Parent = folder end), "service parent locked")

	-- value objects and Changed
	local v = Instance.new("IntValue")
	local got
	v.Changed:Connect(function(new) got = new end)
	v.Value = 4.6
	check(v.Value == 5 and got == 5, "IntValue")
	local sv = Instance.new("StringValue")
	check(sv.Value == "", "StringValue default")
	local prop
	a:GetPropertyChangedSignal("Transparency"):Connect(function() prop = a.Transparency end)
	a.Transparency = 0.25
	check(prop and near(prop, 0.25, 0.01), "GetPropertyChangedSignal")

	-- models
	local m = workspace.Stuff
	local pivot = m:GetPivot()
	m:PivotTo(pivot * CFrame.new(0, 10, 0))
	check(vnear(m:GetPivot().Position, pivot.Position + Vector3.new(0, 10, 0), 0.01), "PivotTo")
	m.PrimaryPart = m.B
	m:SetPrimaryPartCFrame(CFrame.new(50, 5, 50))
	check(vnear(m.B.Position, Vector3.new(50, 5, 50), 0.01), "SetPrimaryPartCFrame")
	local cf, size = m:GetBoundingBox()
	check(typeof(cf) == "CFrame" and size.Magnitude > 1, "GetBoundingBox")

	-- the player
	local plr = Players:GetPlayers()[1]
	check(plr and plr.Name == "Player1" and plr.ClassName == "Player", "player")
	local char = plr.Character
	check(char == workspace.Player1 and Players:GetPlayerFromCharacter(char) == plr, "character")
	local hum = char:FindFirstChildOfClass("Humanoid")
	check(hum and hum.Health == 100 and hum.MaxHealth == 100, "humanoid")
	check(char.HumanoidRootPart:IsA("BasePart") and hum.RootPart == char.HumanoidRootPart, "root part")
	hum.WalkSpeed = 32
	check(hum.WalkSpeed == 32, "WalkSpeed")
	hum.WalkSpeed = 16
	hum.JumpPower = 50
	check(near(hum.JumpPower, 50, 0.1), "JumpPower " .. hum.JumpPower)
	hum:TakeDamage(10)
	check(hum.Health == 90, "TakeDamage")
	hum.Health = 100
	check(not pcall(function() char.Parent = nil end), "character is kept")

	-- raycasts hit the floor
	local r = workspace:Raycast(Vector3.new(0, 50, -30), Vector3.new(0, -100, 0))
	check(r and r.Instance.Name == "Floor" and near(r.Position.Y, 0, 0.05), "Raycast")
	local params = RaycastParams.new()
	params.FilterType = Enum.RaycastFilterType.Exclude
	params.FilterDescendantsInstances = { workspace.Floor }
	check(workspace:Raycast(Vector3.new(0, 50, -30), Vector3.new(0, -100, 0), params) == nil, "Raycast filter")

	-- Lighting and workspace
	Lighting.ClockTime = 6.5
	check(near(Lighting.ClockTime, 6.5) and Lighting.TimeOfDay == "06:30:00", "ClockTime " .. Lighting.TimeOfDay)
	Lighting.TimeOfDay = "18:15:00"
	check(near(Lighting.ClockTime, 18.25), "TimeOfDay")
	check(near(workspace.Gravity, 196.2, 0.01), "Gravity")

	-- modules
	local util = require(RS.Util)
	check(util.double(21) == 42 and util.loaded == 1 and require(RS.Util) == util, "require")
	check(RS:WaitForChild("Util") == RS.Util, "WaitForChild present")

	-- types
	local v3 = Vector3.new(1, 2, 3)
	check(v3 + Vector3.one == Vector3.new(2, 3, 4) and v3 * 2 == Vector3.new(2, 4, 6), "Vector3 ops")
	check(near(Vector3.new(3, 4, 0).Magnitude, 5) and vnear(Vector3.new(0, 5, 0).Unit, Vector3.yAxis), "Magnitude")
	check(v3:Dot(Vector3.new(1, 0, 0)) == 1 and Vector3.xAxis:Cross(Vector3.yAxis) == Vector3.zAxis, "Dot/Cross")
	check(vnear(Vector3.zero:Lerp(Vector3.new(10, 0, 0), 0.3), Vector3.new(3, 0, 0)), "Lerp")
	local cfa = CFrame.new(1, 2, 3) * CFrame.Angles(0.3, 0.2, 0.1)
	check(vnear((cfa * cfa:Inverse()).Position, Vector3.zero), "CFrame inverse")
	check(vnear(cfa:PointToWorldSpace(cfa:PointToObjectSpace(Vector3.new(5, 6, 7))), Vector3.new(5, 6, 7)), "spaces")
	local look = CFrame.lookAt(Vector3.zero, Vector3.new(0, 0, -10))
	check(vnear(look.LookVector, Vector3.new(0, 0, -1)), "lookAt")
	local h, s, val = Color3.fromRGB(255, 0, 0):ToHSV()
	check(near(h, 0) and near(s, 1) and near(val, 1), "ToHSV")
	check(Color3.fromHex("#FF8000"):ToHex():upper() == "FF8000", "hex")
	check(BrickColor.new("Really red").Color == Color3.new(1, 0, 0), "BrickColor color")
	check(Enum.Material.Plastic.Value == 256 and Enum.EasingStyle.Quad.Name == "Quad", "enums")
	check(not pcall(function() Vector3.new().X = 1 end), "Vector3 is immutable")
	check(not pcall(function() Vector3.new = nil end), "constructors are read-only")
	check(TweenInfo.new(2).Time == 2, "TweenInfo")

	-- the Luau bits and library extras
	local t = { 3, 1, 2 }
	check(table.find(t, 2) == 3 and table.find(t, 9) == nil, "table.find")
	check(#string.split("a,b,,c", ",") == 4, "string.split")
	check(math.clamp(5, 0, 3) == 3 and math.sign(-2) == -1 and math.round(2.5) == 3, "math extras")
	local cnt = 0
	for i = 1, 10 do
		if i % 2 == 0 then continue end
		cnt += 1
	end
	check(cnt == 5, "continue")

	-- time, waiting, threads
	local t0 = time()
	local dt = task.wait(0.1)
	check(dt >= 0.09 and time() - t0 >= 0.09, "task.wait " .. dt)
	local order = {}
	task.spawn(function() table.insert(order, "spawn") end)
	task.defer(function() table.insert(order, "defer") end)
	table.insert(order, "after")
	task.wait()
	check(table.concat(order, " ") == "spawn after defer", "task order: " .. table.concat(order, " "))
	local delayed = false
	task.delay(0.05, function() delayed = true end)
	local thread = task.delay(0.05, function() error("cancelled thread ran") end)
	task.cancel(thread)
	wait(0.2)
	check(delayed, "task.delay")
	local hb = RunService.Heartbeat:Wait()
	check(type(hb) == "number" and hb > 0, "Heartbeat:Wait")

	-- events wait
	local ev = Instance.new("BoolValue")
	task.delay(0.05, function() ev.Value = true end)
	local nv = ev.Changed:Wait()
	check(nv == true, "Changed:Wait")

	-- tweens
	local tp = Instance.new("Part", workspace)
	tp.Anchored = true
	tp.Position = Vector3.new(0, 20, 0)
	tp.Transparency = 0
	local tw = game:GetService("TweenService"):Create(tp, TweenInfo.new(0.2, Enum.EasingStyle.Linear),
		{ Transparency = 1, Position = Vector3.new(10, 20, 0) })
	local done = false
	tw.Completed:Connect(function(state) done = state == Enum.PlaybackState.Completed end)
	tw:Play()
	wait(0.1)
	check(tp.Transparency > 0.2 and tp.Transparency < 0.8, "tween midway " .. tp.Transparency)
	wait(0.25)
	check(done and near(tp.Transparency, 1, 0.01) and vnear(tp.Position, Vector3.new(10, 20, 0), 0.01), "tween end")

	-- Debris
	local junk = Instance.new("Part", workspace)
	game:GetService("Debris"):AddItem(junk, 0.05)
	wait(0.2)
	check(junk.Parent == nil, "Debris")

	-- scripts made at run time start when they enter the workspace
	local s = Instance.new("Script")
	check(s.ClassName == "Script", "new Script")

	-- os
	check(type(os.time()) == "number" and type(os.clock()) == "number", "os")
	check(type(tick()) == "number", "tick")
end)

if not ok then
	failures += 1
	warn("API test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "api ok" or ("api failed " .. failures)
result.Parent = workspace
local d = Instance.new("Folder")
d.Name = "api done"
d.Parent = workspace
