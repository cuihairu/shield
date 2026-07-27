-- Test service for LAPI-003 (Registry), LAPI-004 (Message Send),
-- LAPI-005 (Message Call), and LAPI-006 (Context).

local M = {}

local last_sender = nil
local last_method = nil
local last_args = {}
local saved_sender_reader = nil

function M.on_init(args)
    local config = (args and args.config) or {}

    if config.register_alias then
        shield.register(config.register_alias)
    end

    if config.test_case == "timer_sender" then
        shield.timer_once(20, function()
            -- In timer context, ctx is not available, use shield.sender() for backward compatibility
            last_sender = shield.sender() or "__nil__"
        end)
    end
end

local function record_call(ctx, method, args)
    last_sender = ctx.sender
    last_method = method
    last_args = args or {}
end

function M.record(ctx, ...)
    local args = {...}
    record_call(ctx, "record", args)
    return true
end

function M.echo(ctx, msg)
    record_call(ctx, "echo", {msg})
    return msg
end

function M.return_value(ctx)
    return "returned_value"
end

function M.return_false(ctx)
    return false, "return_false_reason"
end

function M.return_nil(ctx)
    return nil
end

function M.throw_error(ctx)
    error("handler_error")
end

function M.multi_return(ctx, ...)
    return ...
end

function M.slow_method(ctx)
    shield.sleep(150)
    return "slow_done"
end

function M.get_last_sender(ctx)
    return last_sender
end

function M.get_last_method(ctx)
    return last_method
end

function M.get_last_args(ctx)
    return last_args
end

function M.query_id(ctx, name)
    local handle, err = shield.query(name)
    if not handle then
        return nil, err and err.code or nil
    end
    return handle:id()
end

function M.register_name(ctx, name)
    local ok, err = shield.register(name)
    return ok, err and err.code or nil, err and err.message or nil
end

function M.unregister_name(ctx, name)
    local ok, err = shield.unregister(name)
    return ok, err and err.code or nil, err and err.message or nil
end

function M.names_snapshot(ctx)
    return shield.names()
end

function M.self_id(ctx)
    local handle = shield.self()
    if not handle then
        return nil
    end
    return handle:id()
end

function M.current_sender(ctx)
    return ctx.sender
end

function M.save_sender_reader(ctx)
    local sender = ctx.sender  -- Capture the value, not the ctx object
    saved_sender_reader = function()
        return sender
    end
    return ctx.sender
end

function M.read_saved_sender(ctx)
    if not saved_sender_reader then
        return nil
    end
    return saved_sender_reader()
end

function M.send_to(ctx, target, method, ...)
    return shield.send(target, method, ...)
end

function M.send_to_query(ctx, name, method, ...)
    local handle, err = shield.query(name)
    if not handle then
        return false, err
    end
    return shield.send(handle, method, ...)
end

function M.call_target(ctx, target, method, ...)
    return shield.call(target, method, ...)
end

function M.call_timeout_target(ctx, timeout_ms, target, method, ...)
    return shield.call_timeout(timeout_ms, target, method, ...)
end

return M
