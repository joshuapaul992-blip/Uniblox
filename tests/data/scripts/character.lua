-- Uniblox: characters, joints, tools and teams (milestone 8C), a Script in
-- ServerScriptService run by tests/test_game.c. The test world has teams
-- Red (auto-assigned) and Blue with a spawn each, a Tool "Sword" in
-- StarterPack, and an anchored "Post" with a loose "Flag" beside it. The
-- test presses 1 (equip), clicks, and presses 1 again. Makes workspace."char
-- ok" when every check passes and "char done" when it finishes.

local failures = 0
local function check(cond, what)
	if not cond then
		failures += 1
		warn("FAIL: " .. what)
	end
end
local function near(a, b, eps)
	return (a - b).Magnitude < (eps or 0.05)
end

local ok, err = pcall(function()
	local Players = game:GetService("Players")
	local player = Players:GetPlayers()[1]
	local char = player.Character
	local hum = char:WaitForChild("Humanoid")
	-- the rig
	check(hum.RigType == Enum.HumanoidRigType.R15, "R15 by default")
	for _, n in { "HumanoidRootPart", "Head", "UpperTorso", "LowerTorso", "RightHand", "LeftFoot", "RightUpperArm" } do
		check(char:FindFirstChild(n) and char[n]:IsA("BasePart"), "part " .. n)
	end
	local neck = char.Head:FindFirstChild("Neck")
	check(neck and neck.ClassName == "Motor6D" and neck.Part0 == char.UpperTorso and neck.Part1 == char.Head, "Neck")
	check(char.RightHand:FindFirstChild("RightGripAttachment") ~= nil, "grip attachment")
	local root = char.HumanoidRootPart
	task.wait(0.1)
	check(math.abs(char.Head.Position.Y - root.Position.Y - 1.7) < 0.1, "head above the root " .. tostring(char.Head.Position - root.Position))
	-- moving the neck moves the head
	local before = char.Head.Position
	neck.C0 = neck.C0 * CFrame.new(0, 1, 0)
	task.wait()
	check(near(char.Head.Position, before + Vector3.new(0, 1, 0), 0.1), "Neck.C0 moves the head")
	neck.C0 = neck.C0 * CFrame.new(0, -1, 0)

	-- teams: auto-assigned to Red, spawned on Red's spawn
	local Teams = game:GetService("Teams")
	check(#Teams:GetTeams() == 2, "GetTeams")
	check(player.Team == Teams.Red and player.TeamColor == BrickColor.new("Bright red") and not player.Neutral, "auto-assigned")
	check(math.abs(root.Position.X - 20) < 3, "spawned at the team's spawn " .. tostring(root.Position))
	check(#Teams.Red:GetPlayers() == 1 and #Teams.Blue:GetPlayers() == 0, "Team:GetPlayers")

	-- a weld from a script: the hat follows the head
	local hat = Instance.new("Part")
	hat.Size = Vector3.new(1, 0.4, 1)
	hat.Name = "Hat"
	hat.Parent = workspace
	local w = Instance.new("Weld")
	w.Part0 = char.Head
	w.Part1 = hat
	w.C0 = CFrame.new(0, 1, 0)
	w.Parent = hat
	task.wait()
	check(near(hat.Position, char.Head.Position + Vector3.new(0, 1, 0), 0.1), "weld places the hat")
	check(w.C0 == CFrame.new(0, 1, 0) and w.Part0 == char.Head, "Weld properties")
	-- a WeldConstraint keeps a loose part where it is against an anchored one
	local post, flag = workspace.Post, workspace.Flag
	local wc = Instance.new("WeldConstraint")
	wc.Part0 = post
	wc.Part1 = flag
	wc.Parent = post
	local offset = flag.Position - post.Position
	post.Position += Vector3.new(0, 5, 0)
	task.wait(0.1)
	check(near(flag.Position - post.Position, offset, 0.1), "WeldConstraint carries " .. tostring(flag.Position - post.Position))
	wc:Destroy()
	task.wait(0.5)
	check(flag.Position.Y < post.Position.Y + offset.Y - 0.5, "let go, it falls")

	-- the tool
	local sword = player.Backpack:WaitForChild("Sword", 2)
	check(sword and sword:IsA("Tool"), "StarterPack copied into the Backpack")
	local equipped, activated, unequipped = false, false, false
	sword.Equipped:Connect(function() equipped = true end)
	sword.Activated:Connect(function() activated = true end)
	sword.Unequipped:Connect(function() unequipped = true end)
	local ready = Instance.new("Folder")
	ready.Name = "char ready"
	ready.Parent = workspace
	local t = 0
	while not equipped and t < 3 do t += task.wait() end
	check(equipped and sword.Parent == char, "equipped with 1")
	task.wait(0.1)
	local grip = char.RightHand:FindFirstChild("RightGrip")
	check(grip and grip.Part1 == sword.Handle, "RightGrip weld")
	check((sword.Handle.Position - char.RightHand.Position).Magnitude < 3, "the handle is in the hand")
	t = 0
	while not unequipped and t < 3 do t += task.wait() end
	check(activated, "Activated by the click")
	check(unequipped and sword.Parent == player.Backpack, "put away with 1 again")
	hum:EquipTool(sword)
	check(sword.Parent == char, "Humanoid:EquipTool")
	hum:UnequipTools()
	check(sword.Parent == player.Backpack, "Humanoid:UnequipTools")

	-- changing team, then respawning, puts you at the new team's spawn
	player.Team = Teams.Blue
	check(player.TeamColor == BrickColor.new("Bright blue"), "TeamColor follows Team")
	hum.Health = 0
	t = 0
	while (hum.Health <= 0 or math.abs(root.Position.X + 20) > 3) and t < 6 do t += task.wait() end
	check(math.abs(root.Position.X + 20) < 3, "respawned at Blue's spawn " .. tostring(root.Position))
	check(player.Backpack:FindFirstChild("Sword") ~= nil, "the backpack refilled")
end)

if not ok then
	failures += 1
	warn("character test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "char ok" or ("char failed " .. failures)
result.Parent = workspace
local done = Instance.new("Folder")
done.Name = "char done"
done.Parent = workspace
