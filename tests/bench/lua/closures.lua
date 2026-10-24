-- Higher-order calls: a closure-built pipeline over an array, 200 times.
local function mapInto(xs, out, n, f) for i = 0, n - 1 do out[i] = f(xs[i]) end end
local function fold(xs, n, acc, f)
  local r = acc
  for i = 0, n - 1 do r = f(r, xs[i]) end
  return r
end
local function adder(k) return function(x) return x + k end end
local n = 100000
local xs, ys = {}, {}
for i = 0, n - 1 do xs[i] = i % 1000; ys[i] = 0 end
local total = 0
for round = 0, 199 do
  mapInto(xs, ys, n, adder(round))
  total = (total + fold(ys, n, 0, function(a, b) return (a + b) % 1000003 end)) % 1000003
end
assert(total == 964063)
