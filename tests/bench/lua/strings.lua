-- Builds strings by concatenation and finds each in a list of those seen, by equality.
local words = { "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta" }
local seen, counts = {}, {}
local total = 0
for i = 0, 1000000 - 1 do
  local w = words[i % 8 + 1] .. "-" .. words[math.floor(i / 8) % 8 + 1]
  local at = 1
  while at <= #seen and seen[at] ~= w do at = at + 1 end
  if at > #seen then seen[at] = w; counts[at] = 0 end
  counts[at] = counts[at] + 1
  total = total + #w
end
assert(#seen == 64 and counts[2] == 15625 and total == 10500000)
