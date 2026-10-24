-- Method calls on metatable instances: a particle system of 1,000 points, 10,000 steps.
local Point = {}
Point.__index = Point
function Point.new(x, y, dx, dy) return setmetatable({ x = x, y = y, dx = dx, dy = dy }, Point) end
function Point:step()
  self.x = self.x + self.dx
  self.y = self.y + self.dy
  if self.x < 0.0 or self.x > 100.0 then self.dx = -self.dx end
  if self.y < 0.0 or self.y > 100.0 then self.dy = -self.dy end
end
function Point:norm() return self.x * self.x + self.y * self.y end
local points = {}
for i = 0, 999 do
  points[#points + 1] = Point.new(i % 100 + 0.0, i % 37 + 0.0, (i % 7) * 0.5 - 1.5, (i % 5) * 0.25 - 0.5)
end
for _ = 1, 10000 do
  for i = 1, #points do points[i]:step() end
end
local total = 0.0
for i = 1, #points do total = total + points[i]:norm() end
assert(math.floor(total) == 7214397)
