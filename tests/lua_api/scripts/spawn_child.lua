-- Child service for Lua-driven shield.spawn tests.
-- on_init stores args; args.init_sleep_ms simulates a slow init (blocking
-- sleep on the spawn worker thread, never on a service actor).

local M = {}

function M.on_init(args)
    M.name = args.name
    M.args = args.args or {}
    local sleep_ms = tonumber(M.args.init_sleep_ms) or 0
    if sleep_ms > 0 then
        shield.sleep(sleep_ms)
    end
    return true
end

function M.ping(ctx)
    return "pong:" .. tostring(M.name)
end

function M.get_args(ctx)
    return M.args
end

return M
