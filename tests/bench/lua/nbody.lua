-- Five bodies under gravity, as tables, for 500,000 steps.
local pi = 3.141592653589793
local solarMass = 4.0 * pi * pi
local year = 365.24
local function body(x, y, z, vx, vy, vz, mass)
  return { x = x, y = y, z = z, vx = vx * year, vy = vy * year, vz = vz * year, mass = mass * solarMass }
end
local bodies = {
  body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
  body(4.84143144246472090, -1.16032004402742839, -0.103622044471123109,
       0.00166007664274403694, 0.00769901118419740425, -0.0000690460016972063023,
       0.000954791938424326609),
  body(8.34336671824457987, 4.12479856412430479, -0.403523417114321381,
       -0.00276742510726862411, 0.00499852801234917238, 0.0000230417297573763929,
       0.000285885980666130812),
  body(12.8943695621391310, -15.1111514016986312, -0.223307578892655734,
       0.00296460137564761618, 0.00237847173959480950, -0.0000296589568540237556,
       0.0000436624404335156298),
  body(15.3796971148509165, -25.9193146099879641, 0.179258772950371181,
       0.00268067772490389322, 0.00162824170038242295, -0.0000951592254519715870,
       0.0000515138902046611451),
}
local sqrt = math.sqrt
local function advance(dt)
  local n = #bodies
  for i = 1, n do
    local b = bodies[i]
    for j = i + 1, n do
      local c = bodies[j]
      local dx, dy, dz = b.x - c.x, b.y - c.y, b.z - c.z
      local d2 = dx * dx + dy * dy + dz * dz
      local mag = dt / (d2 * sqrt(d2))
      local bm, cm = b.mass * mag, c.mass * mag
      b.vx = b.vx - dx * cm; b.vy = b.vy - dy * cm; b.vz = b.vz - dz * cm
      c.vx = c.vx + dx * bm; c.vy = c.vy + dy * bm; c.vz = c.vz + dz * bm
    end
  end
  for i = 1, n do
    local b = bodies[i]
    b.x = b.x + dt * b.vx; b.y = b.y + dt * b.vy; b.z = b.z + dt * b.vz
  end
end
local function energy()
  local e = 0.0
  local n = #bodies
  for i = 1, n do
    local b = bodies[i]
    e = e + 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
    for j = i + 1, n do
      local c = bodies[j]
      local dx, dy, dz = b.x - c.x, b.y - c.y, b.z - c.z
      e = e - b.mass * c.mass / sqrt(dx * dx + dy * dy + dz * dz)
    end
  end
  return e
end
local px, py, pz = 0.0, 0.0, 0.0
for i = 1, #bodies do
  local b = bodies[i]
  px = px + b.vx * b.mass; py = py + b.vy * b.mass; pz = pz + b.vz * b.mass
end
local sun = bodies[1]
sun.vx = -px / solarMass; sun.vy = -py / solarMass; sun.vz = -pz / solarMass
local e0 = energy()
for _ = 1, 500000 do advance(0.01) end
local e1 = energy()
assert(e0 > -0.169075165 and e0 < -0.169075163)
assert(e1 > -0.169096568 and e1 < -0.169096566)
