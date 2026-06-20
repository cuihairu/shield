-- Test service for coroutine-aware shield.call (LAPI-005 coroutine case).
--
-- call_and_record is meant to be invoked via send() (async dispatch) so it
-- runs inside a handler coroutine: shield.call then yields, the callee runs to
-- completion on the same pump, and the caller is resumed with the result.

local M = {}

local last_call_ok = nil
local last_call_result = nil

function M.call_and_record(target, method, ...)
    local ok, result = shield.call(target, method, ...)
    last_call_ok = ok
    last_call_result = result
    return ok
end

function M.greet(name)
    return "hello:" .. tostring(name)
end

function M.get_last_call_ok()
    return last_call_ok
end

function M.get_last_call_result()
    return last_call_result
end

return M
