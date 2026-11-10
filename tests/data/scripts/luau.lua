--!strict
-- Uniblox: Luau 2022 syntax self-test, on a bare Lua state (tests/test_script.c).
-- Type annotations must parse and change nothing; the rest must behave as in Roblox.
local ok, bad = 0, 0
local function check(cond: boolean, name: string): ()
	if cond then ok += 1 else bad += 1; print("FAIL " .. name) end
end

-- type aliases, exported ones and generics: statements that do nothing
type Point = { x: number, y: number }
export type Shape<T> = { kind: string, data: T, area: (self: Shape<T>) -> number }
type Callback = (number, string?) -> (boolean, ...any)
type Map<K, V> = { [K]: V }
type Either = "left" | "right" | nil
type Pack<T...> = (T...) -> T...
type Inter = Point & { z: number }
type Opt = { Point }?
type Weird<T = number> = typeof(setmetatable({}, {}))

-- annotated locals, parameters, varargs, return types, generics
local a: number = 5
local b: string?, c: { [string]: number } = nil, { one = 1 }
local function add(x: number, y: number): number
	return x + y
end
local function first<T>(list: { T }): T?
	return list[1]
end
local function count(...: number): (number, string)
	return select("#", ...), "n"
end
local swap = function<A, B>(x: A, y: B): (B, A) return y, x end
check(add(a, 2) == 7 and first({ 3, 4 }) == 3 and count(1, 2, 3) == 3, "annotated functions")
local s1, s2 = swap(1, "z")
check(s1 == "z" and s2 == 1 and b == nil and c.one == 1, "generic function value")
local t = {}
function t.method<T>(self: any, v: T): T return v end
function t:m2(v: number): number return v * 2 end
check(t:method(9) == 9 and t:m2(4) == 8, "annotated methods")

-- casts
local any: any = "text"
local str = any :: string
check(str == "text" and (5 :: number) + 1 == 6 and ((any :: any) :: string) == "text", "casts")
local nested = { (1 :: number), ("x" :: string) }
check(#nested == 2, "casts in constructors")

-- typed for loops
local sum = 0
for i: number = 1, 3 do sum += i end
for k: string, v: number in pairs({ x = 10, y = 20 }) do sum += v end
check(sum == 36, "typed for loops")

-- `type' and `export' are still names when used as names
local type_ = type
check(type(1) == "number" and type_("s") == "string", "type() still works")
local export = { type = 3 }
check(export.type == 3, "export as a name")

-- if-then-else expressions
local function sign(n: number): string
	return if n > 0 then "+" elseif n < 0 then "-" else "0"
end
check(sign(5) == "+" and sign(-2) == "-" and sign(0) == "0", "if expression")
local x = 10
local y = if x > 5 then x * 2 else x
check(y == 20, "if expression in local")
check((if false then 1 else 2) + 1 == 3 and 1 + if true then 2 else 3 == 3, "if expression in arithmetic")
local calls = 0
local function bump() calls += 1 return calls end
local z = if bump() == 1 then "a" else bump()
check(z == "a" and calls == 1, "if expression evaluates one branch")
local u = { if nil then 1 else 2, (if x then "y" else "n") }
check(u[1] == 2 and u[2] == "y", "if expressions in a table")
local w = if x == 10 then if a == 5 then "both" else "x" else "neither"
check(w == "both", "nested if expressions")
local function multi() return 1, 2 end
local m1 = if true then multi() else 0
check(m1 == 1, "if expression truncates to one value")

-- generalized iteration
local keys, total = 0, 0
for k, v in { 4, 5, 6 } do keys += 1 total += v end
check(keys == 3 and total == 15, "for k, v in array")
local order = {}
for i, v in { "a", "b", "c" } do order[#order + 1] = i .. v end
check(table.concat(order, ",") == "1a,2b,3c", "array order")
local hash = 0
for k, v in { p = 1, q = 2 } do hash += v end
check(hash == 3, "for k, v in hash")
local only = 0
for k in { 1, 2 } do only += k end
check(only == 3, "keys only")
local three = 0
for k, v, extra in { 7 } do if extra == nil then three += v end end
check(three == 7, "extra variables are nil")
for k, v in pairs({ 1 }) do check(v == 1, "pairs still works") end
local callable = setmetatable({}, { __call = function(self, s, c) if not c then return 1 end return nil end })
local seen = 0
for v in callable do seen += v end
check(seen == 1, "callable tables are still called")

-- numbers
check(0b1010 == 10 and 0B11 == 3 and 1_000_000 == 1e6 and 0x_FF == 255 and 1_0.5 == 10.5, "number literals")

-- string escapes
check("\x41\x62" == "Ab" and "\u{48}\u{20AC}" == "H\226\130\172" and "a\z
       b" == "ab", "escapes")
check("\u{1F600}" == "\240\159\152\128", "4-byte \\u")

-- compound assignment and continue (milestone 5) still work with all this
local acc = 0
for i = 1, 10 do
	if i % 2 == 0 then continue end
	acc += i
end
check(acc == 25, "continue")

print(string.format("luau self-test: %d ok, %d failed", ok, bad))
return bad
