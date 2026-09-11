-- [SHIELD_TEST] Service module for spawn-time client RPC binding compilation
-- (architecture-correction M1). Each rpc.routes entry binding must resolve to
-- a function on this module when the service spawns, or the spawn fails with
-- handler_missing. s2c routes only require a non-empty binding name.
local M = {}

function M.on_init(args)
  return true
end

-- Inbound (c2s/bidi) bindings.
function M.do_login(client, request)
  return true
end

function M.do_move(client, request)
  return true
end

function M.chat(client, request)
  return true
end

-- Present but NOT a function: used by the non-function binding failure case.
M.not_callable = { reason = "table, not function" }

-- Server-to-client helper binding: not compiled to a Lua function at spawn
-- time (the API layer registers helpers in M2); only the name matters.
M.push_result = nil

return M
