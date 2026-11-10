-- Uniblox: the wider 2022 API (milestone 8B), a Script in ServerScriptService
-- run by tests/test_game.c. The test world has, near the spawn point:
-- workspace.Button with a ClickDetector straight ahead of the camera,
-- workspace.Chest with a ProximityPrompt (key E) and workspace.Crate with
-- one held for 0.5 s (key F). The test clicks the button, taps E, holds F
-- and jumps. Makes workspace."api2022 ok" when every check passes and
-- workspace."api2022 done" when it finishes either way.

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
local function fails(f)
	return not pcall(f)
end

local ok, err = pcall(function()
	-- classes from the class table, with Roblox's defaults
	local s = Instance.new("Sound")
	check(s.Volume == 0.5 and s.Looped == false and s.PlaybackSpeed == 1 and s:IsA("Sound"), "Sound defaults")
	local light = Instance.new("PointLight")
	check(light.Brightness == 1 and near(light.Range, 8) and light:IsA("Light"), "PointLight")
	local prompt = Instance.new("ProximityPrompt")
	check(prompt.ActionText == "Interact" and prompt.HoldDuration == 0 and prompt.MaxActivationDistance == 10, "ProximityPrompt defaults")
	check(prompt.KeyboardKeyCode == Enum.KeyCode.E and prompt.Style == Enum.ProximityPromptStyle.Default, "ProximityPrompt enums")
	local hl = Instance.new("Highlight")
	check(near(hl.FillTransparency, 0.5) and hl.DepthMode == Enum.HighlightDepthMode.AlwaysOnTop, "Highlight")
	check(Instance.new("ClickDetector").MaxActivationDistance == 32, "ClickDetector default")
	local tool = Instance.new("Tool")
	check(tool.RequiresHandle and tool.CanBeDropped and tool:IsA("BackpackItem"), "Tool")
	check(Instance.new("Atmosphere"):IsA("Atmosphere") and Instance.new("BloomEffect"):IsA("PostEffect"), "effects")
	check(Instance.new("Team").AutoAssignable == true, "Team")
	check(Instance.new("Attachment").Visible == false, "Attachment")
	check(fails(function() s.Volume = "loud" end), "wrong type refused")

	-- value types
	check(Vector3.new(1, 5, 3):Max(Vector3.new(4, 2, 6)) == Vector3.new(4, 5, 6), "Vector3:Max")
	check(Vector3.new(1, 5, 3):Min(Vector3.new(4, 2, 6), Vector3.new(0, 9, 9)) == Vector3.new(0, 2, 3), "Vector3:Min")
	check(Vector3.new(1.5, -1.5, 0):Floor() == Vector3.new(1, -2, 0) and Vector3.new(1.2, -1.2, 0):Ceil() == Vector3.new(2, -1, 0), "Floor/Ceil")
	check(Vector3.new(-3, 0, 2):Sign() == Vector3.new(-1, 0, 1), "Sign")
	check(near(Vector3.xAxis:Angle(Vector3.yAxis), math.pi / 2), "Angle")
	check(near(Vector3.xAxis:Angle(Vector3.yAxis, Vector3.new(0, 0, -1)), -math.pi / 2), "signed Angle")
	check(Vector3.fromNormalId(Enum.NormalId.Front) == Vector3.new(0, 0, -1) and Vector3.fromAxis(Enum.Axis.Y) == Vector3.yAxis, "fromNormalId/fromAxis")
	local axis, angle = CFrame.fromAxisAngle(Vector3.new(0, 1, 0), 0.5):ToAxisAngle()
	check(near(angle, 0.5) and (axis - Vector3.new(0, 1, 0)).Magnitude < 1e-4, "ToAxisAngle")
	check(CFrame.new(1, 2, 3):FuzzyEq(CFrame.new(1, 2, 3.000001)) and not CFrame.new():FuzzyEq(CFrame.new(1, 0, 0)), "CFrame:FuzzyEq")
	local r = Region3.new(Vector3.new(4, 0, 0), Vector3.new(0, 2, 2))
	check(r.Size == Vector3.new(4, 2, 2) and r.CFrame.Position == Vector3.new(2, 1, 1) and typeof(r) == "Region3", "Region3")
	check(r:ExpandToGrid(4).Size == Vector3.new(4, 4, 4), "ExpandToGrid")
	local v16 = Vector3int16.new(1, 2, 3) + Vector3int16.new(10, 20, 30)
	check(v16.X == 11 and v16.Z == 33 and typeof(v16) == "Vector3int16" and Vector3int16.new(40000).X == -25536, "Vector3int16")
	check(Vector2int16.new(3, 4).Y == 4, "Vector2int16")
	local pp = PhysicalProperties.new(Enum.Material.Ice)
	check(near(pp.Friction, 0.02) and near(pp.Density, 0.919) and typeof(pp) == "PhysicalProperties", "PhysicalProperties(Ice)")
	check(PhysicalProperties.new(2, 0.5, 0.3).Elasticity == 0.3 or near(PhysicalProperties.new(2, 0.5, 0.3).Elasticity, 0.3), "PhysicalProperties.new")
	local faces = Faces.new(Enum.NormalId.Top, Enum.NormalId.Left)
	check(faces.Top and faces.Left and not faces.Bottom, "Faces")
	local axes = Axes.new(Enum.Axis.X, Enum.NormalId.Top)
	check(axes.X and axes.Y and not axes.Z and axes.Left, "Axes")
	local op = OverlapParams.new()
	check(typeof(op) == "OverlapParams" and op.MaxParts == 0 and op.FilterType == Enum.RaycastFilterType.Blacklist, "OverlapParams")

	-- spatial queries (Roblox axes: the button is at 0, 2, -4)
	local inBox = workspace:GetPartBoundsInBox(CFrame.new(0, 2, -4), Vector3.new(1, 1, 1))
	check(#inBox == 1 and inBox[1] == workspace.Button, "GetPartBoundsInBox " .. #inBox)
	local inRadius = workspace:GetPartBoundsInRadius(Vector3.new(0, 2, -4), 1)
	check(table.find(inRadius, workspace.Button) ~= nil, "GetPartBoundsInRadius")
	op.FilterDescendantsInstances = { workspace.Floor }
	local around = workspace:GetPartBoundsInRadius(Vector3.new(0, 0, 0), 4, op)
	check(table.find(around, workspace.Floor) == nil and table.find(around, workspace.Chest) ~= nil, "OverlapParams filter")
	op.MaxParts = 1
	check(#workspace:GetPartBoundsInRadius(Vector3.new(0, 0, 0), 20, op) == 1, "MaxParts")
	local touching = workspace:GetPartsInPart(workspace.Button)
	check(table.find(touching, workspace.Button) == nil, "GetPartsInPart leaves the part out")
	local reg = workspace:FindPartsInRegion3(Region3.new(Vector3.new(-1, 1, -5), Vector3.new(1, 3, -3)))
	check(#reg == 1 and reg[1] == workspace.Button, "FindPartsInRegion3")
	check(#workspace:FindPartsInRegion3WithIgnoreList(Region3.new(Vector3.new(-1, 1, -5), Vector3.new(1, 3, -3)), { workspace.Button }) == 0, "WithIgnoreList")

	-- services that answer offline
	local policy = game:GetService("PolicyService"):GetPolicyInfoForPlayerAsync(game.Players:GetPlayers()[1])
	check(policy.AreAdsAllowed == false and type(policy.AllowedExternalLinkReferences) == "table", "PolicyService")
	check(game:GetService("BadgeService"):UserHasBadgeAsync(1, 2) == false, "BadgeService")
	local tr = game:GetService("LocalizationService"):GetTranslatorForPlayerAsync(game.Players:GetPlayers()[1])
	check(tr:Translate(workspace, "Hello") == "Hello" and game:GetService("LocalizationService").RobloxLocaleId == "en-us", "LocalizationService")
	local path = game:GetService("PathfindingService"):CreatePath()
	path:ComputeAsync(Vector3.new(0, 0, 0), Vector3.new(0, 0, 10))
	local wps = path:GetWaypoints()
	check(path.Status == Enum.PathStatus.Success and #wps == 4 and wps[#wps].Position == Vector3.new(0, 0, 10), "PathfindingService " .. #wps)
	check(#game:GetService("GroupService"):GetGroupsAsync(1) == 0, "GroupService")

	-- what the test does to the world
	local seen = {}
	local player = game.Players:GetPlayers()[1]
	local cd = workspace.Button.ClickDetector
	cd.MouseHoverEnter:Connect(function(p) if p == player then seen.hover = true end end)
	cd.MouseClick:Connect(function(p) if p == player then seen.click = (seen.click or 0) + 1 end end)
	local chest = workspace.Chest.ProximityPrompt
	chest.PromptShown:Connect(function(input) seen.shown = input == Enum.ProximityPromptInputType.Keyboard end)
	chest.Triggered:Connect(function(p) if p == player then seen.triggered = (seen.triggered or 0) + 1 end end)
	chest.TriggerEnded:Connect(function() seen.ended = true end)
	local crate = workspace.Crate.ProximityPrompt
	crate.PromptButtonHoldBegan:Connect(function() seen.holdBegan = time() end)
	crate.Triggered:Connect(function() seen.held = time() - (seen.holdBegan or 0) end)
	crate.PromptButtonHoldEnded:Connect(function() seen.holdEnded = true end)
	local PPS = game:GetService("ProximityPromptService")
	PPS.PromptTriggered:Connect(function(pr, p) if p == player then seen.service = (seen.service or 0) + 1 end end)
	local hum = player.Character:WaitForChild("Humanoid")
	local states = {}
	hum.StateChanged:Connect(function(old, new) states[#states + 1] = new.Name end)
	hum.Jumping:Connect(function(active) if active then seen.jumping = true end end)
	hum.FreeFalling:Connect(function(active) if active then seen.falling = true end end)
	local ready = Instance.new("Folder")
	ready.Name = "api2022 ready"
	ready.Parent = workspace

	local t = 0
	while t < 6 and not (seen.click and seen.triggered and seen.held and seen.holdEnded and seen.jumping and #states >= 3 and hum:GetState() == Enum.HumanoidStateType.Running) do
		t += task.wait()
	end
	check(seen.hover, "MouseHoverEnter")
	check(seen.click == 1, "MouseClick " .. tostring(seen.click))
	check(seen.shown, "PromptShown")
	check(seen.triggered == 1 and seen.ended, "Triggered and TriggerEnded " .. tostring(seen.triggered))
	check(seen.held and seen.held >= 0.45 and seen.held < 0.9, "held for HoldDuration " .. tostring(seen.held))
	check(seen.holdEnded, "PromptButtonHoldEnded")
	check(seen.service == 2, "ProximityPromptService.PromptTriggered " .. tostring(seen.service))
	check(seen.jumping and seen.falling, "Jumping and FreeFalling")
	check(table.find(states, "Jumping") and table.find(states, "Freefall") and table.find(states, "Landed") and states[#states] == "Running",
		"StateChanged " .. table.concat(states, ","))
	check(game:GetService("CoreGui") == nil or #game:GetService("CoreGui"):GetChildren() >= 0, "CoreGui")
end)

if not ok then
	failures += 1
	warn("2022 API test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "api2022 ok" or ("api2022 failed " .. failures)
result.Parent = workspace
local done = Instance.new("Folder")
done.Name = "api2022 done"
done.Parent = workspace
