-- Escape-time iteration over a 1200x1200 grid, at most 50 steps a point.
local size = 1200
local function inside(x, y)
  local m = 2.0 / size
  local cr = x * m - 1.5
  local ci = y * m - 1.0
  local zr, zi = cr, ci
  for _ = 1, 50 do
    local zrq, ziq = zr * zr, zi * zi
    if zrq + ziq > 4.0 then return 0 end
    zi = zr * zi * 2.0 + ci
    zr = zrq - ziq + cr
  end
  return 1
end
local count = 0
for y = 0, size - 1 do
  for x = 0, size - 1 do count = count + inside(x, y) end
end
assert(count == 571628)
