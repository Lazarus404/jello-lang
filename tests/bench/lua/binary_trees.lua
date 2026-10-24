-- Allocates and walks complete binary trees: short-lived tables for the collector.
local function make(depth)
  if depth == 0 then return { left = false, right = false } end
  return { left = make(depth - 1), right = make(depth - 1) }
end
local function check(node)
  local l, r = node.left, node.right
  if not l or not r then return 1 end
  return 1 + check(l) + check(r)
end
local maxDepth = 16
local total = check(make(maxDepth + 1))
local longLived = make(maxDepth)
for depth = 4, maxDepth, 2 do
  local iterations = 2 ^ (maxDepth - depth + 4)
  for _ = 1, iterations do total = total + check(make(depth)) end
end
total = total + check(longLived)
assert(total == 14985902)
