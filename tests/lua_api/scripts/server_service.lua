-- shield_server P0 test service.
--
-- Every test entry point takes the dispatch ctx first (invoke_coroutine
-- prepend_ctx, same convention as player_service.lua). Watch callbacks
-- record into M.calls so the C++ test can poll for asynchronous deliveries.
local M = {}

-- Per-VM watch callback log: {{state = ...}, ...}
M.calls = {}
M.watch_id = nil

function M.on_init(args)
    M.init_args = args or {}
end

-- ---------------------------------------------------------------------------
-- test entry points (ctx first)
-- ---------------------------------------------------------------------------

-- Read-only info surface: one entry per shield.server accessor.
function M.info(ctx)
    return {state = shield.server.state(),
            uptime = shield.server.uptime(),
            version = shield.server.version(),
            node_id = shield.server.node_id(),
            started_at = shield.server.started_at()}
end

function M.config(ctx)
    return shield.server.config()
end

-- Returns {ok, code} for a set_state attempt (nil + error table on failure).
function M.do_set_state(ctx, name)
    local ok, err = shield.server.set_state(name)
    if not ok then
        return {ok = false, code = type(err) == 'table' and err.code
                    or tostring(err)}
    end
    return {ok = true}
end

-- Registers the recorder callback; re-watching keeps the original one.
function M.do_watch(ctx)
    local id, err = shield.server.watch(function(ctx2, new_state)
        table.insert(M.calls, {state = new_state})
    end)
    if not id then
        return {ok = false, code = type(err) == 'table' and err.code
                    or tostring(err)}
    end
    M.watch_id = id
    return {ok = true, id = id}
end

function M.do_unwatch(ctx)
    return shield.server.unwatch(M.watch_id)
end

-- The same call with a float-typed id: the facade unwraps Lua numbers that
-- carry the float subtype through its double branch.
function M.do_unwatch_float(ctx, id)
    return shield.server.unwatch(id)
end

-- delay: nil (immediate), a number, or a string to probe invalid_argument.
function M.do_shutdown(ctx, delay)
    local ok, err = shield.server.shutdown(delay)
    if not ok then
        return {ok = false, code = type(err) == 'table' and err.code
                    or tostring(err)}
    end
    return {ok = true}
end

function M.calls_json(ctx)
    return M.calls
end

-- Stub probe: drives every facade entry (compiled-out build) and reports
-- the stable error code each one produces.
function M.stub_probe(ctx)
    local codes = {}
    local function probe(name, fn)
        local _, err = fn()
        codes[name] = type(err) == 'table' and err.code or tostring(err)
    end
    probe('state', shield.server.state)
    probe('uptime', shield.server.uptime)
    probe('version', shield.server.version)
    probe('node_id', shield.server.node_id)
    probe('started_at', shield.server.started_at)
    probe('config', shield.server.config)
    probe('set_state', function() return shield.server.set_state('running') end)
    probe('shutdown', function() return shield.server.shutdown(0) end)
    probe('watch', function() return shield.server.watch(function() end) end)
    probe('unwatch', function() return shield.server.unwatch(1) end)
    return codes
end

return M
