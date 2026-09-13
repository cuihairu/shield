-- shield_server P0: watch from the module top level.
--
-- At load time there is no dispatch frame and no registry entry yet, so the
-- facade must reject the registration with the stable invalid_argument
-- error instead of crashing or silently registering a watcher. A failed
-- assert here fails the spawn, which is what the C++ case asserts on.
local ok, err = shield.server.watch(function(ctx, new_state) end)
assert(ok == nil, 'top-level watch must fail')
assert(type(err) == 'table' and err.code == 'invalid_argument',
       'expected invalid_argument, got ' .. tostring(err))

local M = {}
function M.on_init(args) end
return M
