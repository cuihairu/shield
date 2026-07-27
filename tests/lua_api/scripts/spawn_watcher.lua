-- Watcher recording panic reports from spawn_panic_service.

local M = {}

local events = {}

function M.record_panic(ctx, reason, ctype)
    events[#events + 1] = { reason = tostring(reason), type = tostring(ctype) }
    return true
end

function M.get_events(ctx)
    return events
end

return M
