-- The spectral norm of an infinite matrix, by the power method, at N = 800.
local function a(i, j)
  local ij = i + j
  return 1.0 / (ij * (ij + 1) / 2 + i + 1)
end
local function times(x, y, n)
  for i = 0, n - 1 do
    local s = 0.0
    for j = 0, n - 1 do s = s + a(i, j) * x[j] end
    y[i] = s
  end
end
local function timesTransposed(x, y, n)
  for i = 0, n - 1 do
    local s = 0.0
    for j = 0, n - 1 do s = s + a(j, i) * x[j] end
    y[i] = s
  end
end
local n = 800
local u, v, t = {}, {}, {}
for i = 0, n - 1 do u[i] = 1.0; v[i] = 0.0; t[i] = 0.0 end
for _ = 1, 10 do
  times(u, t, n); timesTransposed(t, v, n)
  times(v, t, n); timesTransposed(t, u, n)
end
local vbv, vv = 0.0, 0.0
for i = 0, n - 1 do vbv = vbv + u[i] * v[i]; vv = vv + v[i] * v[i] end
local r = math.sqrt(vbv / vv)
assert(r > 1.2742241 and r < 1.2742242)
