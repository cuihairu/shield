-- Test service for coroutine-aware shield.call (LAPI-005 coroutine case).
--
-- call_and_record is meant to be invoked via send() (async dispatch) so it
-- runs inside a handler coroutine: shield.call then yields, the callee runs to
-- completion on the same pump, and the caller is resumed with the result.

local M = {}

local last_call_ok = nil
local last_call_result = nil
local last_call_extra = nil

function M.call_and_record(target, method, ...)
    local ok, result, extra = shield.call(target, method, ...)
    last_call_ok = ok
    last_call_result = result
    last_call_extra = extra
    return ok
end

function M.call_timeout_and_record(timeout_ms, target, method, ...)
    local ok, result, extra = shield.call_timeout(timeout_ms, target, method, ...)
    last_call_ok = ok
    last_call_result = result
    last_call_extra = extra
    return ok
end

function M.greet(name)
    return "hello:" .. tostring(name)
end

function M.greet_multi(name)
    return "hello:" .. tostring(name), "extra:" .. tostring(name)
end

function M.greet_slow(name)
    -- Yields the callee coroutine via shield.sleep; the caller stays suspended
    -- until this sleeps-and-resumes, then returns its value.
    shield.sleep(40)
    return "slow:" .. tostring(name)
end

function M.get_last_call_ok()
    return last_call_ok
end

function M.get_last_call_result()
    return last_call_result
end

function M.get_last_call_extra()
    return last_call_extra
end

return M
