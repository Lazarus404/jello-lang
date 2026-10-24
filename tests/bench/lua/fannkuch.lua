-- Pancake flips over every permutation of 10 items.
local function fannkuch(n)
  local perm, perm1, count = {}, {}, {}
  for i = 0, n - 1 do perm1[i] = i; perm[i] = 0; count[i] = 0 end
  local maxFlips, checksum, sign, r = 0, 0, 1, n
  while true do
    while r ~= 1 do count[r - 1] = r; r = r - 1 end
    for i = 0, n - 1 do perm[i] = perm1[i] end
    local flips = 0
    local k = perm[0]
    while k ~= 0 do
      local lo, hi = 0, k
      while lo < hi do perm[lo], perm[hi] = perm[hi], perm[lo]; lo = lo + 1; hi = hi - 1 end
      flips = flips + 1
      k = perm[0]
    end
    if flips > maxFlips then maxFlips = flips end
    checksum = checksum + sign * flips
    sign = -sign
    while true do
      if r == n then return maxFlips * 1000000 + checksum end
      local first = perm1[0]
      for i = 0, r - 1 do perm1[i] = perm1[i + 1] end
      perm1[r] = first
      count[r] = count[r] - 1
      if count[r] > 0 then break end
      r = r + 1
    end
  end
end
assert(fannkuch(10) == 38073196)
