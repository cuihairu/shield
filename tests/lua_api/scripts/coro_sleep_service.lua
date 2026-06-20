-- Test service for coroutine-aware shield.sleep (LAPI-007 sleep cases).
--
-- The handler yields via shield.sleep and is resumed by the runtime's sleep
-- timer, so the C++ harness can verify that a sleeping handler does not block
-- the pump and that execution continues after the deadline.

local M = {}

local events = {}

function M.sleep_and_mark(index, ms)
    shield.sleep(ms)
    table.insert(events, {index = index, at = shield.now()})
    return true
end

function M.event_count()
    return #events
end

function M.last_index()
    local last = events[#events]
    return last and last.index or nil
end

return M
