-- Uniblox GUI API test: a LocalScript in a ScreenGui in StarterGui, run by
-- tests/test_game.c (it has to be copied into the PlayerGui to run). The
-- test clicks the button "Btn" and presses E about a second in, on an
-- 800x636 screen. Makes workspace."gui ok" when every check passes and
-- workspace."gui done" when it finishes either way.

local failures = 0
local function check(cond, what)
	if not cond then
		failures += 1
		warn("FAIL: " .. what)
	end
end
local function near(a, b, eps)
	return math.abs(a - b) < (eps or 1e-3)
end
local function waitfor(f, seconds)
	local t = 0
	while not f() and t < (seconds or 3) do
		t += wait()
	end
	return f()
end

local ok, err = pcall(function()
	local Players = game:GetService("Players")
	local player = Players.LocalPlayer
	local gui = script.Parent
	check(gui.ClassName == "ScreenGui" and gui.Parent == player.PlayerGui, "copied into the PlayerGui")
	check(player:WaitForChild("PlayerGui").ClassName == "PlayerGui", "PlayerGui")
	check(game:GetService("StarterGui"):FindFirstChild("Hud") ~= gui, "StarterGui keeps its own")

	-- value types
	local u = UDim2.new(0.5, 10, 0, 20)
	check(u.X.Scale == 0.5 and u.X.Offset == 10 and u.Y.Offset == 20 and u.Width == u.X, "UDim2 parts")
	check(u + UDim2.fromOffset(5, 5) == UDim2.new(0.5, 15, 0, 25), "UDim2 +")
	check(UDim2.fromScale(1, 1) - UDim2.new(1, 0, 1, 0) == UDim2.new(), "UDim2 -")
	check(tostring(UDim2.new(0.5, 10, 0, 20)) == "{0.5, 10}, {0, 20}", "UDim2 tostring")
	check(u:Lerp(UDim2.new(1, 30, 0, 20), 0.5) == UDim2.new(0.75, 20, 0, 20), "UDim2 Lerp")
	check(UDim.new(1, 2) + UDim.new(0, 3) == UDim.new(1, 5), "UDim")
	local v = Vector2.new(3, 4)
	check(v.Magnitude == 5 and near(v.Unit.X, 0.6) and (v * 2).Y == 8 and (v / 2).X == 1.5, "Vector2")
	check(v:Dot(Vector2.new(1, 0)) == 3 and Vector2.zero + Vector2.one == Vector2.one, "Vector2 2")
	check(Rect.new(1, 2, 5, 8).Width == 4 and Rect.new(Vector2.new(0, 0), v).Max == v, "Rect")
	check(NumberRange.new(2, 4).Max == 4 and NumberRange.new(3).Min == 3, "NumberRange")
	local ns = NumberSequence.new({ NumberSequenceKeypoint.new(0, 1), NumberSequenceKeypoint.new(1, 0) })
	check(#ns.Keypoints == 2 and ns.Keypoints[2].Value == 0, "NumberSequence")
	local cs = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
	check(cs.Keypoints[1].Value == Color3.new(1, 0, 0) and cs.Keypoints[2].Time == 1, "ColorSequence")
	check(not pcall(function() NumberSequence.new({ NumberSequenceKeypoint.new(0.5, 1) }) end), "bad sequence")
	check(typeof(u) == "UDim2" and typeof(v) == "Vector2" and typeof(cs) == "ColorSequence", "typeof")
	check(Font.fromEnum(Enum.Font.SourceSansBold).Weight == Enum.FontWeight.Bold, "Font.fromEnum")

	-- Roblox's enums
	check(Enum.Font.SourceSans.Value == 3 and Enum.KeyCode.E.Value == 101, "enum values")
	check(tostring(Enum.TextXAlignment.Left) == "Enum.TextXAlignment.Left", "enum tostring")
	check(Enum.UserInputType.MouseButton1 ~= Enum.UserInputType.MouseButton2, "enum items")
	check(#Enum:GetEnums() > 50 and #Enum.SortOrder:GetEnumItems() == 3, "GetEnums")

	-- a new Frame has Roblox's defaults
	local f = Instance.new("Frame")
	check(f.Size == UDim2.new() and f.BorderSizePixel == 1 and f.Visible and f.ZIndex == 1, "Frame defaults")
	check(f.BackgroundColor3 == Color3.fromRGB(163, 162, 165) or near(f.BackgroundColor3.R, 0.639), "Frame colour")
	check(f:IsA("GuiObject") and f:IsA("GuiBase2d") and not f:IsA("GuiButton"), "IsA")
	check(not pcall(function() f.Size = 5 end), "wrong type refused")
	check(not pcall(function() f.AbsoluteSize = Vector2.new() end), "read-only refused")
	check(f.AbsoluteSize == Vector2.new(0, 0), "not shown: no size")
	f.Size = UDim2.new(0, 200, 0, 100)
	f.Position = UDim2.new(0.5, 0, 0, 50)
	f.AnchorPoint = Vector2.new(0.5, 0)
	f.Transparency = 0.25
	check(f.BackgroundTransparency == 0.25, "Transparency alias")
	f.Parent = gui
	check(f.AbsoluteSize == Vector2.new(200, 100), "AbsoluteSize " .. tostring(f.AbsoluteSize))
	check(f.AbsolutePosition == Vector2.new(300, 50), "AbsolutePosition " .. tostring(f.AbsolutePosition))
	local cam = workspace.CurrentCamera
	check(cam.ViewportSize == Vector2.new(800, 636), "ViewportSize " .. tostring(cam.ViewportSize))
	local inset = game:GetService("GuiService"):GetGuiInset()
	check(inset == Vector2.new(0, 36), "GuiInset")
	check(gui.AbsoluteSize == Vector2.new(800, 600), "ScreenGui AbsoluteSize")

	-- size changes are reported
	local sizes = 0
	f:GetPropertyChangedSignal("AbsoluteSize"):Connect(function() sizes += 1 end)
	f.Size = UDim2.new(0, 220, 0, 100)
	check(f.AbsoluteSize.X == 220, "AbsoluteSize follows")
	check(waitfor(function() return sizes > 0 end, 1), "AbsoluteSize changed signal")

	-- text
	local t = Instance.new("TextLabel")
	check(t.Text == "Label" and t.TextSize == 8 and t.Font == Enum.Font.Legacy, "TextLabel defaults")
	t.Text = "abcd"
	t.TextSize = 20
	t.Size = UDim2.new(0, 100, 0, 30)
	t.Parent = f
	check(t.TextBounds == Vector2.new(40, 20), "TextBounds " .. tostring(t.TextBounds))
	t.Font = Enum.Font.SourceSansBold
	check(t.FontFace.Weight == Enum.FontWeight.Bold, "Font sets FontFace")
	t.FontFace = Font.new("rbxasset://fonts/families/SourceSansPro.json")
	check(t.Font == Enum.Font.SourceSans, "FontFace sets Font")
	t.RichText = true
	t.Text = "<b>Hi</b> &amp; you"
	check(t.ContentText == "Hi & you", "ContentText")
	local size = game:GetService("TextService"):GetTextSize("hello", 14, Enum.Font.SourceSans, Vector2.new(1000, 1000))
	check(size == Vector2.new(35, 14), "GetTextSize " .. tostring(size))

	-- layouts
	local list = Instance.new("UIListLayout")
	list.Padding = UDim.new(0, 4)
	local holder = Instance.new("Frame", gui)
	holder.Size = UDim2.fromOffset(100, 300)
	list.Parent = holder
	for i = 1, 3 do
		local r = Instance.new("Frame")
		r.Size = UDim2.new(1, 0, 0, 20)
		r.LayoutOrder = i
		r.Parent = holder
	end
	check(list.AbsoluteContentSize == Vector2.new(100, 68), "AbsoluteContentSize " .. tostring(list.AbsoluteContentSize))

	-- tweens of GUI values
	check(f:TweenSize(UDim2.new(0, 300, 0, 100), Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 0.2, true), "TweenSize")
	local tw = game:GetService("TweenService"):Create(holder, TweenInfo.new(0.2), { Position = UDim2.new(0, 50, 0, 0) })
	tw:Play()
	wait(0.5)
	check(f.Size == UDim2.new(0, 300, 0, 100), "TweenSize done " .. tostring(f.Size))
	check(holder.Position == UDim2.new(0, 50, 0, 0), "tweened UDim2 " .. tostring(holder.Position))

	-- value objects
	local iv = Instance.new("IntValue")
	local seen
	iv.Changed:Connect(function(x) seen = x end)
	iv.Value = 2.6
	check(iv.Value == 3, "IntValue rounds")
	check(waitfor(function() return seen == 3 end, 1), "IntValue Changed")
	local ov = Instance.new("ObjectValue")
	ov.Value = f
	check(ov.Value == f, "ObjectValue")

	-- remotes (a server Script answers) and bindables
	local RS = game:GetService("ReplicatedStorage")
	local got
	RS.Ping.OnClientEvent:Connect(function(x) got = x end)
	RS.Ping:FireServer(21)
	check(waitfor(function() return got == 42 end, 1), "RemoteEvent round trip")
	local sum, name = RS.Ask:InvokeServer(2, 3)
	check(sum == 5 and name == player.Name, "RemoteFunction")
	local be = Instance.new("BindableEvent")
	local fired
	be.Event:Connect(function(a, b) fired = a .. b end)
	be:Fire("x", "y")
	check(waitfor(function() return fired == "xy" end, 1), "BindableEvent")
	local bf = Instance.new("BindableFunction")
	bf.OnInvoke = function(a) return a * 10 end
	check(bf:Invoke(4) == 40, "BindableFunction")

	-- a text box takes the keyboard
	local box = Instance.new("TextBox", gui)
	box.Size = UDim2.fromOffset(100, 20)
	box:CaptureFocus()
	check(box:IsFocused() and game:GetService("UserInputService"):GetFocusedTextBox() == box, "CaptureFocus")
	box:ReleaseFocus()
	check(not box:IsFocused(), "ReleaseFocus")

	-- the test clicks the button and presses E
	local btn = gui:WaitForChild("Btn")
	check(btn:IsA("GuiButton") and btn.AutoButtonColor, "button")
	-- the client UI (milestone 9A): what SetCore, SetCoreGuiEnabled and Chat reach
	local SG = game:GetService("StarterGui")
	check(SG:GetCoreGuiEnabled(Enum.CoreGuiType.PlayerList), "the player list starts on")
	SG:SetCoreGuiEnabled(Enum.CoreGuiType.PlayerList, false)
	check(not SG:GetCoreGuiEnabled(Enum.CoreGuiType.PlayerList) and SG:GetCoreGuiEnabled(Enum.CoreGuiType.Chat), "SetCoreGuiEnabled")
	SG:SetCoreGuiEnabled(Enum.CoreGuiType.All, true)
	check(SG:GetCoreGuiEnabled(Enum.CoreGuiType.PlayerList), "SetCoreGuiEnabled(All)")
	SG:SetCoreGuiEnabled(Enum.CoreGuiType.Health, false)
	SG:SetCore("ChatMakeSystemMessage", { Text = "[System] hello", Color = Color3.fromRGB(255, 200, 0) })
	SG:SetCore("SendNotification", { Title = "Hi", Text = "there", Duration = 3 })
	SG:SetCore("ResetButtonCallback", false)
	local Chat = game:GetService("Chat")
	Chat:Chat(workspace.Door, "a bubble")
	check(Chat:FilterStringForBroadcast("plain words", player) == "plain words", "chat filter")
	check(Chat.BubbleChatEnabled == true, "BubbleChatEnabled")
	local clicks, activated, entered, keyed, mouseX = 0, nil, false, false, nil
	btn.MouseButton1Click:Connect(function() clicks += 1 end)
	btn.Activated:Connect(function(input, n) activated = input.UserInputType end)
	btn.MouseEnter:Connect(function(x, y) entered = true end)
	local UIS = game:GetService("UserInputService")
	UIS.InputBegan:Connect(function(input, processed)
		if input.KeyCode == Enum.KeyCode.E and not processed then keyed = true end
	end)
	local mouse = player:GetMouse()
	mouse.Button1Down:Connect(function() mouseX = mouse.X end)
	local doneflag = Instance.new("Folder")
	doneflag.Name = "gui ready"
	doneflag.Parent = workspace
	check(waitfor(function() return clicks > 0 and keyed end, 4), "click and key")
	check(clicks == 1 and activated == Enum.UserInputType.MouseButton1 and entered, "button events")
	check(UIS:GetMouseLocation() == Vector2.new(60, 66), "GetMouseLocation " .. tostring(UIS:GetMouseLocation()))
	check(mouseX == nil, "the click went to the button, not the world")
	check(mouse.X == 60 and mouse.Y == 30, "Mouse.X/Y")
end)

if not ok then
	failures += 1
	warn("GUI test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "gui ok" or ("gui failed " .. failures)
result.Parent = workspace
local d = Instance.new("Folder")
d.Name = "gui done"
d.Parent = workspace
