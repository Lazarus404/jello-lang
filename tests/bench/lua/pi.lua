-- A tight float loop with no calls: 100,000,000 Leibniz terms.
local sum, sign = 0.0, 1.0
for i = 0, 100000000 - 1 do
  sum = sum + sign / (2.0 * i + 1.0)
  sign = -sign
end
local pi = 4.0 * sum
assert(pi > 3.1415926 and pi < 3.1415927)
