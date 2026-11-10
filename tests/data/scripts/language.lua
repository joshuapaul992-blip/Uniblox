-- Uniblox: Lua language self-test (runs in the host tests and, inside the
-- game, in the Wine smoke test). check(cond, name) counts results.
local ok, bad = 0, 0
local function check(cond, name)
  if cond then ok = ok + 1 else bad = bad + 1; print("FAIL " .. name) end
end

-- numbers and formatting (our printf and strtod)
check(tostring(1/3) == "0.33333333333333", "tostring 1/3")
check(tostring(10) == "10" and tostring(-0.5) == "-0.5" and tostring(1e100) == "1e+100", "tostring ints")
check(tostring(2^53) == "9.007199254741e+15", "tostring 2^53")
check(tonumber("0x1F") == 31 and tonumber("  12.5  ") == 12.5 and tonumber("1e3") == 1000, "tonumber")
check(tonumber("abc") == nil and tonumber("nan") == nil and tonumber("10", 2) == 2, "tonumber nil")
check(string.format("%5.2f|%d|%x|%s|%q", 3.14159, 42, 255, "hi", "a\n") == " 3.14|42|ff|hi|\"a\\\n\"", "format")
check(string.format("%g %e", 0.0001, 12345.678) == "0.0001 1.234568e+04", "format g e")
check(math.floor(-3.5) == -4 and math.ceil(-3.5) == -3 and math.abs(-2) == 2, "floor ceil")
check(math.max(1, 5, 3) == 5 and math.min(4, -2) == -2 and 7 % 3 == 1 and -7 % 3 == 2, "max min mod")
check(math.abs(math.sin(math.pi / 6) - 0.5) < 1e-15 and math.abs(math.cos(0) - 1) < 1e-15, "trig")
check(math.abs(math.sqrt(2) ^ 2 - 2) < 1e-15 and 2 ^ 10 == 1024 and math.exp(0) == 1, "pow sqrt exp")
check(math.abs(math.log(math.exp(2)) - 2) < 1e-15 and math.log10(1000) == 3, "log")
check(math.fmod(7, 3) == 1 and math.huge > 1e308 and -math.huge < -1e308, "fmod huge")
check(select("#", math.modf(3.7)) == 2 and math.floor(math.atan2(1, 1) * 4 / math.pi + 0.5) == 1, "modf atan2")

-- strings and patterns
check(("hello"):upper() == "HELLO" and ("abc"):rep(3) == "abcabcabc" and ("abc"):sub(2) == "bc", "string basics")
check(string.find("hello world", "o w") == 5 and select(2, string.gsub("aaa", "a", "b")) == 3, "find gsub")
check(string.match("key = value", "(%w+)%s*=%s*(%w+)") == "key", "match")
local words = {}
for w in string.gmatch("one two three", "%a+") do words[#words + 1] = w end
check(#words == 3 and words[3] == "three", "gmatch")
check(("x"):rep(0) == "" and (""):rep(1e9) == "", "rep edge cases")

-- tables, closures, metatables, coroutines
local t = { 5, 3, 8, 1 }
table.sort(t)
check(table.concat(t, ",") == "1,3,5,8", "sort concat")
table.insert(t, 2, 9)
check(t[2] == 9 and table.remove(t) == 8 and #t == 4, "insert remove")
local counter = (function() local n = 0; return function() n = n + 1; return n end end)()
counter(); counter()
check(counter() == 3, "closure")
local v = setmetatable({}, { __add = function(a, b) return "added" end, __index = function(_, k) return k * 2 end })
check(v + v == "added" and v[21] == 42, "metatables")
local co = coroutine.create(function(a) local b = coroutine.yield(a + 1); return b * 2 end)
local _, r1 = coroutine.resume(co, 1)
local _, r2 = coroutine.resume(co, 10)
check(r1 == 2 and r2 == 20 and coroutine.status(co) == "dead", "coroutines")
check(not pcall(error, "x") and select(2, pcall(error, { code = 7 })).code == 7, "pcall error")
check(select(2, pcall(function() local x = nil; return x.y end)):find("attempt to index") ~= nil, "error message")

-- Luau syntax: compound assignment and continue
local n = 10
n += 5; n -= 3; n *= 2; n /= 4; n %= 4; n ^= 2
check(n == 4, "compound locals")
local s = "a"
s ..= "b"
s ..= 1
check(s == "ab1", "compound concat")
local tab = { x = 1, list = { 10 } }
tab.x += 1
tab.list[1] *= 3
tab["x"] += 10
check(tab.x == 12 and tab.list[1] == 30, "compound indexed")
g_compound = 1
g_compound += 1
check(g_compound == 2, "compound global")
local sum = 0
for i = 1, 10 do
  if i % 2 == 0 then continue end
  sum += i
end
check(sum == 25, "continue in numeric for")
local evens = 0
for _, x in ipairs({ 1, 2, 3, 4 }) do
  if x % 2 == 1 then
    continue
  end
  evens += 1
end
check(evens == 2, "continue in generic for")
local i, w = 0, 0
while i < 5 do
  i += 1
  if i == 3 then continue end
  w += 1
end
check(i == 5 and w == 4, "continue in while")
local fs = {}
for j = 1, 3 do
  local captured = j
  fs[j] = function() return captured end
  if j == 2 then continue end
end
check(fs[1]() == 1 and fs[2]() == 2 and fs[3]() == 3, "continue closes upvalues")
local continue = 5         -- still usable as a name
continue = continue + 1
check(continue == 6, "continue as a name")
check(not loadstring or true, "noop")

print(string.format("language self-test: %d ok, %d failed", ok, bad))
return bad
