-- The primes below 4,000,000, sieved in a flag array, five times.
local function sieve(m)
  local flags = {}
  for i = 0, m - 1 do flags[i] = false end
  local count = 0
  for i = 2, m - 1 do
    if not flags[i] then
      count = count + 1
      for j = i + i, m - 1, i do flags[j] = true end
    end
  end
  return count
end
for _ = 1, 5 do assert(sieve(4000000) == 283146) end
