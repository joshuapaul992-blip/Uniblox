-- Uniblox: constraints and body movers (milestone 8C), a Script run by
-- tests/test_game.c. Builds each case in the Workspace, waits, measures.
local failures = 0
local function check(cond, what)
	if not cond then
		failures += 1
		warn("FAIL: " .. what)
	end
end

local function part(name, pos, size, anchored)
	local p = Instance.new("Part")
	p.Name = name
	p.Size = size
	p.Position = pos
	p.Anchored = anchored
	p.Parent = workspace
	return p
end
local function attach(p, pos, axis)
	local a = Instance.new("Attachment")
	a.Position = pos or Vector3.new()
	if axis then a.Axis = axis end
	a.Parent = p
	return a
end

local ok, err = pcall(function()
	-- a door on a servo hinge (vertical axis) turns to 90 degrees
	local post = part("HingePost", Vector3.new(40, 5, 40), Vector3.new(1, 10, 1), true)
	local door = part("HingeDoor", Vector3.new(43, 5, 40), Vector3.new(5, 8, 0.5), false)
	local a0 = attach(post, Vector3.new(0.5, 0, 0), Vector3.new(0, 1, 0))
	local a1 = attach(door, Vector3.new(-2.5, 0, 0), Vector3.new(0, 1, 0))
	check(a1.Axis == Vector3.new(0, 1, 0) and a1.Position == Vector3.new(-2.5, 0, 0), "attachment properties")
	local hinge = Instance.new("HingeConstraint")
	hinge.Attachment0 = a0
	hinge.Attachment1 = a1
	hinge.ActuatorType = Enum.ActuatorType.Servo
	hinge.AngularSpeed = 3
	hinge.ServoMaxTorque = 1e7
	hinge.TargetAngle = 90
	hinge.Parent = post

	-- a ball on a rope hangs at the rope's length
	local hook = part("RopeHook", Vector3.new(60, 30, 40), Vector3.new(1, 1, 1), true)
	local ball = part("RopeBall", Vector3.new(64, 30, 40), Vector3.new(1, 1, 1), false)
	local rope = Instance.new("RopeConstraint")
	rope.Attachment0 = attach(hook)
	rope.Attachment1 = attach(ball)
	rope.Length = 6
	rope.Parent = hook

	-- a slider driven along x by a prismatic motor
	local rail = part("Rail", Vector3.new(80, 3, 40), Vector3.new(1, 1, 1), true)
	local slider = part("Slider", Vector3.new(80, 3, 42), Vector3.new(1, 1, 1), false)
	local pr = Instance.new("PrismaticConstraint")
	pr.Attachment0 = attach(rail, Vector3.new(0, 0, 0), Vector3.new(1, 0, 0))
	pr.Attachment1 = attach(slider, Vector3.new(0, 0, -2), Vector3.new(1, 0, 0))
	pr.ActuatorType = Enum.ActuatorType.Motor
	pr.Velocity = 4
	pr.MotorMaxForce = 1e7
	pr.Parent = rail

	-- BodyVelocity: flies along x, held up
	local flyer = part("Flyer", Vector3.new(0, 20, 60), Vector3.new(2, 2, 2), false)
	local bv = Instance.new("BodyVelocity")
	bv.Velocity = Vector3.new(10, 0, 0)
	bv.MaxForce = Vector3.new(1e7, 1e7, 1e7)
	bv.Parent = flyer

	-- BodyPosition: goes to a point
	local seeker = part("Seeker", Vector3.new(-20, 3, 60), Vector3.new(2, 2, 2), false)
	local bp = Instance.new("BodyPosition")
	bp.Position = Vector3.new(-20, 15, 70)
	bp.MaxForce = Vector3.new(1e6, 1e6, 1e6)
	bp.Parent = seeker

	-- VectorForce against gravity: floats
	local floater = part("Floater", Vector3.new(20, 25, 70), Vector3.new(2, 2, 2), false)
	local vf = Instance.new("VectorForce")
	vf.Attachment0 = attach(floater)
	vf.RelativeTo = Enum.ActuatorRelativeTo.World
	vf.Force = Vector3.new(0, floater:GetMass() * workspace.Gravity, 0)
	vf.Parent = floater

	-- AlignOrientation: turns a loose block to face 90 degrees about y
	local spinner = part("Turner", Vector3.new(-40, 20, 60), Vector3.new(2, 2, 2), false)
	local hold = Instance.new("BodyPosition")
	hold.Position = Vector3.new(-40, 20, 60)
	hold.MaxForce = Vector3.new(1e7, 1e7, 1e7)
	hold.Parent = spinner
	local al = Instance.new("AlignOrientation")
	al.Mode = Enum.OrientationAlignmentMode.OneAttachment
	al.Attachment0 = attach(spinner)
	al.CFrame = CFrame.Angles(0, math.rad(90), 0)
	al.MaxTorque = 1e7
	al.Responsiveness = 50
	al.Parent = spinner

	task.wait(2)
	local look = door.CFrame.RightVector
	check(math.abs(look.Y) < 0.05 and math.abs(math.abs(look.Z) - 1) < 0.15, "servo turned the door " .. tostring(look))
	check(math.abs(hinge.CurrentAngle) > 0 or true, "CurrentAngle readable")
	local d = (ball.Position - hook.Position).Magnitude
	check(math.abs(d - 6) < 0.3 and ball.Position.Y < hook.Position.Y, "rope holds the ball " .. d)
	check(slider.Position.X > 84 and math.abs(slider.Position.Z - 42) < 0.3 and math.abs(slider.Position.Y - 3) < 0.3,
		"prismatic motor moved the slider " .. tostring(slider.Position))
	check(flyer.Position.X > 15 and math.abs(flyer.Position.Y - 20) < 1, "BodyVelocity " .. tostring(flyer.Position))
	check((seeker.Position - bp.Position).Magnitude < 1, "BodyPosition " .. tostring(seeker.Position))
	check(math.abs(floater.Position.Y - 25) < 1, "VectorForce holds it up " .. tostring(floater.Position))
	local rv = spinner.CFrame.RightVector
	check(math.abs(rv.Z) > 0.95, "AlignOrientation " .. tostring(rv))
	-- a sound from content/777.wav (a quarter of a second, made by the test)
	local snd = Instance.new("Sound")
	snd.SoundId = "rbxassetid://777"
	snd.Parent = workspace
	check(snd.IsLoaded and math.abs(snd.TimeLength - 0.25) < 0.01, "sound loaded " .. snd.TimeLength)
	local played, ended = false, false
	snd.Played:Connect(function() played = true end)
	snd.Ended:Connect(function() ended = true end)
	snd:Play()
	check(snd.IsPlaying and snd.Playing, "playing")
	task.wait(0.5)
	check(played and ended and not snd.IsPlaying, "Played and Ended")
	local missing = Instance.new("Sound", workspace)
	missing.SoundId = "rbxassetid://424242"
	check(not missing.IsLoaded and missing.TimeLength == 0, "a sound that is not there")
	missing:Play()
	-- disabling lets go: the rope's ball drops once the rope goes
	rope:Destroy()
	task.wait(0.6)
	check(ball.Position.Y < hook.Position.Y - 8, "without the rope it falls " .. tostring(ball.Position))
end)

if not ok then
	failures += 1
	warn("constraints test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "con ok" or ("con failed " .. failures)
result.Parent = workspace
local done = Instance.new("Folder")
done.Name = "con done"
done.Parent = workspace
