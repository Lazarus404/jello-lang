-- Three-argument recursion: tak(24, 16, 8), twenty times.
local function tak(x, y, z)
  if y < x then return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y)) end
  return z
end
local r = 0
for _ = 1, 20 do r = r + tak(24, 16, 8) end
assert(r == 180)
