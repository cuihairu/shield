-- Parent service for Lua-driven shield.spawn tests.
-- spawn_and_record is meant to be invoked via send() so it runs inside a
-- handler coroutine: shield.spawn then suspends the coroutine while the
-- spawn worker runs the child's on_init off-actor.

local M = {}

local last_ok = nil
local last_id = nil
local last_node = nil
local last_err = nil

function M.spawn_and_record(ctx, script, name, extra_args)
    local opts = { name = name, args = extra_args or {} }
    local h, err = shield.spawn(script, opts)
    last_ok = (h ~= nil)
    last_err = err
    last_id = nil
    last_node = nil
    if h then
        last_id = h:id()
        last_node = h:node()
    end
    return last_ok
end

function M.spawn_in_init_recorded(ctx)
    return last_ok, last_id
end

function M.ping(ctx)
    return "pong"
end

function M.self_node(ctx)
    local me = shield.self()
    if not me then
        return nil
    end
    return me:node()
end

function M.exit_now(ctx, reason)
    shield.exit(reason or "normal")
    return true
end

function M.get_last_ok(ctx)
    return last_ok
end

function M.get_last_id(ctx)
    return last_id
end

function M.get_last_node(ctx)
    return last_node
end

function M.get_last_err(ctx)
    return last_err
end

return M
