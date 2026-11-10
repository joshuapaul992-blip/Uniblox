-- Uniblox: Luau's standard library additions (2022), run as a Script by
-- tests/test_game.c. The test platform's clock is fixed: local time is
-- 2000-01-01 00:00:00 and UTC is one hour behind. Makes workspace."lib ok"
-- when every check passes and workspace."lib done" when it finishes.

local failures = 0
local function check(cond, what)
	if not cond then
		failures += 1
		warn("FAIL: " .. what)
	end
end
local function near(a, b, eps)
	return math.abs(a - b) < (eps or 1e-9)
end
local function fails(f, pattern)
	local ok, err = pcall(f)
	return not ok and (pattern == nil or string.find(tostring(err), pattern, 1, true) ~= nil)
end

local ok, err = pcall(function()
	-- bit32
	check(bit32.band(0xFF, 0x0F) == 0x0F and bit32.bor(1, 2, 4) == 7 and bit32.bxor(5, 3) == 6, "band/bor/bxor")
	check(bit32.band() == 0xFFFFFFFF and bit32.bnot(0) == 0xFFFFFFFF and bit32.bnot(-1) == 0, "defaults and bnot")
	check(bit32.lshift(1, 31) == 0x80000000 and bit32.lshift(1, 32) == 0 and bit32.rshift(0x80000000, 31) == 1, "shifts")
	check(bit32.lshift(4, -1) == 2 and bit32.rshift(-1, 28) == 15, "negative shifts and wrap")
	check(bit32.arshift(0x80000000, 4) == 0xF8000000 and bit32.arshift(0x40, 4) == 4, "arshift")
	check(bit32.lrotate(0x80000001, 1) == 3 and bit32.rrotate(3, 1) == 0x80000001, "rotates")
	check(bit32.extract(0xABCD, 4, 8) == 0xBC and bit32.replace(0, 0xF, 28, 4) == 0xF0000000, "extract/replace")
	check(bit32.btest(6, 2) and not bit32.btest(4, 2), "btest")
	check(bit32.countlz(1) == 31 and bit32.countlz(0) == 32 and bit32.countrz(8) == 3, "countlz/countrz")
	check(bit32.byteswap(0x11223344) == 0x44332211, "byteswap")
	check(fails(function() bit32.extract(1, 30, 4) end), "extract out of range")

	-- utf8
	check(utf8.char(72, 0x20AC, 0x1F600) == "H\226\130\172\240\159\152\128", "utf8.char")
	local s = "aé€😀"
	check(utf8.len(s) == 4 and #s == 10, "utf8.len")
	local cps = {}
	for p, c in utf8.codes(s) do cps[#cps + 1] = p .. ":" .. c end
	check(table.concat(cps, ",") == "1:97,2:233,4:8364,7:128512", "utf8.codes " .. table.concat(cps, ","))
	local a, b = utf8.codepoint(s, 2, 4)
	check(a == 233 and b == 8364, "utf8.codepoint")
	check(utf8.offset(s, 3) == 4 and utf8.offset(s, -1) == 7 and utf8.offset(s, 5) == 11, "utf8.offset")
	local bad, at = utf8.len("ab\255c")
	check(bad == nil and at == 3, "utf8.len invalid")
	local n = 0
	for _ in string.gmatch(s, utf8.charpattern) do n += 1 end
	check(n == 4, "charpattern")
	local g = {}
	for i, j in utf8.graphemes("e\204\129x") do g[#g + 1] = i .. "-" .. j end
	check(table.concat(g, ",") == "1-3,4-4", "graphemes " .. table.concat(g, ","))
	check(utf8.nfcnormalize("abc") == "abc", "normalize")

	-- table
	local t = { 1, 2, 3, 4, 5 }
	table.move(t, 2, 4, 1)
	check(table.concat(t, ",") == "2,3,4,4,5", "table.move down")
	t = { 1, 2, 3 }
	table.move(t, 1, 3, 2)
	check(table.concat(t, ",") == "1,1,2,3", "table.move up")
	local dst = table.move({ 7, 8 }, 1, 2, 3, { 1, 2 })
	check(table.concat(dst, ",") == "1,2,7,8", "table.move to another table")
	local f = table.freeze({ x = 1, 2 })
	check(table.isfrozen(f) and not table.isfrozen({}), "isfrozen")
	check(fails(function() f.x = 2 end, "readonly"), "frozen: set")
	check(fails(function() f.y = 2 end, "readonly"), "frozen: new key")
	check(fails(function() table.insert(f, 3) end, "readonly"), "frozen: insert")
	check(fails(function() rawset(f, "z", 1) end, "readonly"), "frozen: rawset")
	check(fails(function() setmetatable(f, {}) end, "readonly"), "frozen: setmetatable")
	check(f.x == 1 and f[1] == 2, "frozen: read")
	check(fails(function() table.freeze(setmetatable({}, { __metatable = "no" })) end), "freeze a protected table")
	local c = table.clone(f)
	c.x = 5
	check(c.x == 5 and not table.isfrozen(c) and f.x == 1, "clone of a frozen table")
	local mt = { __index = function() return "dflt" end }
	check(getmetatable(table.clone(setmetatable({}, mt))) == mt, "clone keeps the metatable")
	check(table.find({ "a", "b" }, "b") == 2 and #table.create(3, 0) == 3, "find and create")

	-- math
	check(math.noise(1, 2, 3) == 0 and math.noise(5) == 0, "noise is 0 on the lattice")
	check(near(math.noise(1.25, 2.5, -3.75), -0.40540552139282227), "Perlin noise " .. math.noise(1.25, 2.5, -3.75))
	check(near(math.noise(-0.3, 0.7, 0.1), -0.4167942013675007), "Perlin noise 2")
	local lo, hi = 1, -1
	for i = 1, 200 do
		local v = math.noise(i * 0.37, i * 0.11, i * 0.07)
		lo = math.min(lo, v)
		hi = math.max(hi, v)
	end
	check(lo >= -1 and hi <= 1 and hi - lo > 0.4, "noise range")
	check(near(math.log(8, 2), 3) and near(math.log(100, 10), 2) and near(math.log(math.exp(1)), 1), "math.log base")
	check(math.clamp(5, 1, 3) == 3 and math.clamp(-1, 0, 1) == 0, "clamp")
	check(fails(function() math.clamp(1, 3, 2) end, "max must be greater"), "clamp min > max")
	check(math.round(2.5) == 3 and math.round(-2.5) == -3 and math.sign(-4) == -1, "round and sign")

	-- os, tick, DateTime
	check(os.time() == 946681200, "os.time() " .. os.time())
	check(os.time({ year = 2020, month = 2, day = 29, hour = 13, min = 5, sec = 9 }) == 1582981509, "os.time(t)")
	check(os.time({ year = 2020, month = 13, day = 1, hour = 0 }) == os.time({ year = 2021, month = 1, day = 1, hour = 0 }), "os.time normalises")
	check(os.time({ year = 1970, month = 1, day = 1 }) == 12 * 3600, "os.time noon by default")
	check(fails(function() os.time({ year = 2000 }) end, "month"), "os.time missing field")
	check(os.date("!%Y-%m-%d %H:%M:%S", 1582981509) == "2020-02-29 13:05:09", "os.date UTC")
	check(os.date("%H:%M", 0) == "01:00", "os.date local " .. os.date("%H:%M", 0))
	local d = os.date("!*t", 1582981509)
	check(d.year == 2020 and d.month == 2 and d.day == 29 and d.wday == 7 and d.yday == 60 and d.isdst == false, "os.date *t")
	check(os.date("!%A %B %d %j %p %I %y", 1582981509) == "Saturday February 29 060 PM 01 20", "os.date names")
	check(os.date("!%c", 0) == "Thu Jan  1 00:00:00 1970", "os.date %c " .. os.date("!%c", 0))
	check(os.date("*t").year == 2000, "os.date now (local)")
	check(fails(function() os.date("%Q") end, "invalid conversion"), "os.date bad format")
	check(math.floor(tick()) == 946681200, "tick() is UTC time")

	local now = DateTime.now()
	check(typeof(now) == "DateTime" and now.UnixTimestamp == 946681200 and now.UnixTimestampMillis == 946681200000, "DateTime.now")
	local dt = DateTime.fromUnixTimestamp(1582981509)
	check(dt:ToIsoDate() == "2020-02-29T13:05:09Z", "ToIsoDate " .. dt:ToIsoDate())
	local u = dt:ToUniversalTime()
	check(u.Year == 2020 and u.Month == 2 and u.Day == 29 and u.Hour == 13 and u.Minute == 5 and u.Second == 9, "ToUniversalTime")
	check(dt:ToLocalTime().Hour == 14, "ToLocalTime")
	check(DateTime.fromUniversalTime(2020, 2, 29, 13, 5, 9) == dt, "fromUniversalTime and ==")
	check(DateTime.fromLocalTime(2020, 2, 29, 14, 5, 9) == dt, "fromLocalTime")
	check(DateTime.fromUnixTimestampMillis(1582981509250):ToUniversalTime().Millisecond == 250, "millis")
	check(DateTime.fromIsoDate("2020-02-29T13:05:09Z") == dt and DateTime.fromIsoDate("2020-02-29T15:05:09+02:00") == dt, "fromIsoDate")
	check(DateTime.fromIsoDate("2020-02-29T13:05:09.5Z").UnixTimestampMillis == 1582981509500, "fromIsoDate fraction")
	check(DateTime.fromIsoDate("yesterday") == nil, "fromIsoDate bad")
	check(DateTime.fromUnixTimestamp(0) < dt and dt <= dt, "DateTime order")
	check(dt:FormatUniversalTime("dddd, MMMM Do YYYY, h:mm:ss A", "en-us") == "Saturday, February 29th 2020, 1:05:09 PM",
		"FormatUniversalTime " .. dt:FormatUniversalTime("dddd, MMMM Do YYYY, h:mm:ss A", "en-us"))
	check(dt:FormatUniversalTime("LL [at] HH:mm", "en-us") == "February 29, 2020 at 13:05", "Format LL and literal")
	check(dt:FormatLocalTime("YY-MM-DD HH", "en-us") == "20-02-29 14", "FormatLocalTime")
	check(fails(function() DateTime.now = nil end), "DateTime is read-only")

	-- Random
	local r1, r2 = Random.new(42), Random.new(42)
	local same = true
	for i = 1, 20 do
		if r1:NextInteger(1, 100) ~= r2:NextInteger(1, 100) then same = false end
	end
	check(same, "Random is deterministic")
	check(typeof(r1) == "Random", "typeof Random")
	local seen, inrange = {}, true
	for i = 1, 400 do
		local v = r1:NextInteger(1, 6)
		if v < 1 or v > 6 or v ~= math.floor(v) then inrange = false end
		seen[v] = true
	end
	check(inrange and #seen == 6, "NextInteger range")
	local x = r1:NextNumber()
	local y = r1:NextNumber(5, 10)
	check(x >= 0 and x < 1 and y >= 5 and y < 10, "NextNumber")
	local uv = r1:NextUnitVector()
	check(typeof(uv) == "Vector3" and math.abs(uv.Magnitude - 1) < 1e-4, "NextUnitVector")
	local c1 = r1:Clone()
	check(c1:NextNumber() == r1:NextNumber(), "Clone")
	local deck = { 1, 2, 3, 4, 5, 6, 7, 8 }
	r1:Shuffle(deck)
	local sum = 0
	for _, v in deck do sum += v end
	check(#deck == 8 and sum == 36, "Shuffle")
	check(fails(function() r1:NextInteger(5, 1) end), "NextInteger empty")

	-- coroutines
	local co = coroutine.create(function() coroutine.yield(1) error("never") end)
	coroutine.resume(co)
	check(coroutine.status(co) == "suspended", "suspended")
	check(coroutine.close(co) == true and coroutine.status(co) == "dead", "close")
	local okr, msg = coroutine.resume(co)
	check(not okr and string.find(msg, "dead", 1, true), "closed is dead")
	local co2 = coroutine.create(function() error("boom", 0) end)
	coroutine.resume(co2)
	local okc, cmsg = coroutine.close(co2)
	check(okc == false and cmsg == "boom", "close a failed coroutine")
	check(coroutine.isyieldable(), "isyieldable in a script")
	local waited = 0
	local th = task.spawn(function() task.wait(0.05) waited += 1 end)
	coroutine.close(th)
	task.wait(0.15)
	check(waited == 0, "a closed thread does not wake up")
	local closer = coroutine.wrap(function() return coroutine.close(coroutine.running()) end)
	check(fails(closer), "cannot close the running coroutine")

	-- debug
	local function where() return debug.info(1, "sln") end
	local src, line, name = where()
	check(type(src) == "string" and line > 0 and name == "where", "debug.info level " .. tostring(name))
	local function three(a, b, ...) end
	local np, va = debug.info(three, "a")
	check(np == 2 and va == true, "debug.info a")
	check(debug.info(three, "f") == three and debug.info(three, "n") == nil, "debug.info f/n")
	local tb = debug.traceback("msg")
	check(string.sub(tb, 1, 4) == "msg\n" and string.find(tb, ":%d+") ~= nil, "traceback " .. tb)
	debug.profilebegin("x")
	debug.profileend()
	check(debug.getinfo == nil and debug.sethook == nil and debug.getlocal == nil, "no unsafe debug")

	-- the base
	local p = newproxy(true)
	getmetatable(p).__index = function(_, k) return k .. "!" end
	check(type(p) == "userdata" and p.hi == "hi!", "newproxy")
	check(getmetatable(newproxy()) == nil, "newproxy without a metatable")
	check(rawlen({ 1, 2 }) == 2 and rawlen("abc") == 3, "rawlen")
	check(type(gcinfo()) == "number", "gcinfo")
	check(string.format("%s %s %s", nil, true, { } ~= nil) == "nil true true", "%s takes anything")
	check(string.format("%s", Vector3.new(1, 2, 3)) == tostring(Vector3.new(1, 2, 3)), "%s uses __tostring")
	check(string.format("%5s|%-4s|", "ab", 1) == "   ab|1   |", "%s widths")

	-- string.pack
	local packed = string.pack("<i4 I2 b z s1 d", -2, 65535, -1, "hi", "xyz", 1.5)
	check(#packed == 4 + 2 + 1 + 3 + 4 + 8, "pack size " .. #packed)
	local v1, v2, v3, v4, v5, v6, nextpos = string.unpack("<i4 I2 b z s1 d", packed)
	check(v1 == -2 and v2 == 65535 and v3 == -1 and v4 == "hi" and v5 == "xyz" and v6 == 1.5 and nextpos == #packed + 1, "unpack")
	check(string.pack(">I2", 0x1234) == "\x12\x34" and string.unpack(">I2", "\x12\x34") == 0x1234, "big-endian")
	check(string.packsize("i4 i8 !4 i2") == 14 and fails(function() string.pack("i1", 200) end), "packsize and overflow")
	check(string.unpack("f", string.pack("f", 0.5)) == 0.5, "float")

	-- HttpService JSON
	local Http = game:GetService("HttpService")
	check(Http:JSONEncode({ 1, 2, "three", true }) == '[1,2,"three",true]', "encode array")
	check(Http:JSONEncode({}) == "[]" and Http:JSONEncode("a\"b\n") == '"a\\"b\\n"', "encode empty and escapes")
	check(Http:JSONEncode({ a = 1 }) == '{"a":1}' and Http:JSONEncode(0.5) == "0.5", "encode object and number")
	local round = Http:JSONDecode(Http:JSONEncode({ name = "x", list = { 1, 2, { deep = true } }, n = -3.25e2 }))
	check(round.name == "x" and round.list[3].deep == true and round.n == -325, "round trip")
	local dec = Http:JSONDecode('{"a":[1,null,3],"s":"\\u00e9\\ud83d\\ude00","e":1E2}')
	check(dec.a[1] == 1 and dec.a[2] == nil and dec.a[3] == 3 and dec.s == "é😀" and dec.e == 100, "decode")
	check(fails(function() Http:JSONDecode("{bad}") end, "Can't parse JSON"), "decode error")
	check(fails(function() Http:JSONDecode("[1,]") end), "decode trailing comma")
	local cyc = {}
	cyc.self = cyc
	check(fails(function() Http:JSONEncode(cyc) end), "encode cycle")
	check(Http:UrlEncode("a b&c=é") == "a%20b%26c%3D%C3%A9", "UrlEncode")
	check(#Http:GenerateGUID(false) == 36, "GenerateGUID")

	-- attributes (the test world gives workspace.Tagged @Speed = 16 and the tags Coin, Spin)
	local tagged = workspace:WaitForChild("Tagged")
	check(tagged:GetAttribute("Speed") == 16 and tagged:GetAttribute("Nope") == nil, "attribute from the place")
	local folder = Instance.new("Folder")
	local changes, named = 0, 0
	folder.AttributeChanged:Connect(function(n) if n == "Hp" then changes += 1 end end)
	folder:GetAttributeChangedSignal("Hp"):Connect(function() named += 1 end)
	folder:SetAttribute("Hp", 100)
	folder:SetAttribute("Hp", 100) -- the same value: no event
	folder:SetAttribute("Where", Vector3.new(1, 2, 3))
	folder:SetAttribute("Tint", Color3.new(1, 0, 0))
	folder:SetAttribute("Label", "hi")
	folder:SetAttribute("On", true)
	folder:SetAttribute("Size", UDim2.new(0.5, 1, 0, 2))
	check(folder:GetAttribute("Hp") == 100 and folder:GetAttribute("Where") == Vector3.new(1, 2, 3), "SetAttribute")
	check(folder:GetAttribute("Tint") == Color3.new(1, 0, 0) and folder:GetAttribute("Size") == UDim2.new(0.5, 1, 0, 2), "attribute types")
	local all = folder:GetAttributes()
	local count = 0
	for k, v in all do count += 1 end
	check(count == 6 and all.Label == "hi" and all.On == true, "GetAttributes")
	folder:SetAttribute("Hp", nil)
	check(folder:GetAttribute("Hp") == nil and folder:GetAttributes().Hp == nil, "remove an attribute")
	task.wait()
	check(changes == 2 and named == 2, "AttributeChanged " .. changes .. " " .. named)
	check(fails(function() folder:SetAttribute("bad name", 1) end), "bad attribute name")
	check(fails(function() folder:SetAttribute("RBXThing", 1) end), "reserved attribute name")
	check(fails(function() folder:SetAttribute("F", function() end) end, "not a supported attribute type"), "bad attribute type")
	check(folder:Clone():GetAttribute("Label") == "hi", "Clone keeps attributes")

	-- tags and CollectionService
	local CS = game:GetService("CollectionService")
	check(tagged:HasTag("Coin") and CS:HasTag(tagged, "Spin") and not tagged:HasTag("Other"), "tags from the place")
	local tags = tagged:GetTags()
	check(#tags == 2 and tags[1] == "Coin" and tags[2] == "Spin", "GetTags")
	check(#CS:GetTagged("Coin") == 1 and CS:GetTagged("Coin")[1] == tagged, "GetTagged")
	local added, removed = {}, {}
	CS:GetInstanceAddedSignal("Coin"):Connect(function(i) added[#added + 1] = i end)
	CS:GetInstanceRemovedSignal("Coin"):Connect(function(i) removed[#removed + 1] = i end)
	local coin = Instance.new("Part")
	coin:AddTag("Coin")
	check(coin:HasTag("Coin") and #CS:GetTagged("Coin") == 1, "not in the game: not tagged yet")
	coin.Parent = workspace
	check(#CS:GetTagged("Coin") == 2, "GetTagged after parenting")
	CS:AddTag(coin, "Coin") -- again: nothing
	coin:AddTag("Gold")
	local alltags = CS:GetAllTags()
	check(table.find(alltags, "Gold") and table.find(alltags, "Spin") and #alltags == 3, "GetAllTags")
	CS:RemoveTag(tagged, "Coin")
	check(not tagged:HasTag("Coin") and tagged:HasTag("Spin"), "RemoveTag")
	coin:Destroy()
	task.wait()
	check(#added == 1 and added[1] == coin, "InstanceAdded " .. #added)
	check(#removed == 2, "InstanceRemoved " .. #removed)
	check(#CS:GetTagged("Coin") == 0, "nothing left with the tag")
end)

if not ok then
	failures += 1
	warn("library test error: " .. tostring(err))
end
local result = Instance.new("Folder")
result.Name = failures == 0 and "lib ok" or ("lib failed " .. failures)
result.Parent = workspace
local done = Instance.new("Folder")
done.Name = "lib done"
done.Parent = workspace
