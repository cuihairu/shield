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
            last_sender = shield.sender() or "__nil__"
        end)
    end
end

local function record_call(method, args)
    last_sender = shield.sender()
    last_method = method
    last_args = args or {}
end

function M.record(...)
    local args = {...}
    record_call("record", args)
    return true
end

function M.echo(msg)
    record_call("echo", {msg})
    return msg
end

function M.return_value()
    return "returned_value"
end

function M.return_false()
    return false, "return_false_reason"
end

function M.return_nil()
    return nil
end

function M.throw_error()
    error("handler_error")
end

function M.multi_return(...)
    return ...
end

function M.slow_method()
    shield.sleep(150)
    return "slow_done"
end

function M.get_last_sender()
    return last_sender
end

function M.get_last_method()
    return last_method
end

function M.get_last_args()
    return last_args
end

function M.query_id(name)
    local handle, err = shield.query(name)
    if not handle then
        return nil, err and err.code or nil
    end
    return handle:id()
end

function M.register_name(name)
    local ok, err = shield.register(name)
    return ok, err and err.code or nil, err and err.message or nil
end

function M.unregister_name(name)
    local ok, err = shield.unregister(name)
    return ok, err and err.code or nil, err and err.message or nil
end

function M.names_snapshot()
    return shield.names()
end

function M.self_id()
    local handle = shield.self()
    if not handle then
        return nil
    end
    return handle:id()
end

function M.current_sender()
    return shield.sender()
end

function M.save_sender_reader()
    saved_sender_reader = function()
        return shield.sender()
    end
    return shield.sender()
end

function M.read_saved_sender()
    if not saved_sender_reader then
        return nil
    end
    return saved_sender_reader()
end

function M.send_to(target, method, ...)
    return shield.send(target, method, ...)
end

function M.send_to_query(name, method, ...)
    local handle, err = shield.query(name)
    if not handle then
        return false, err
    end
    return shield.send(handle, method, ...)
end

function M.call_target(target, method, ...)
    return shield.call(target, method, ...)
end

function M.call_timeout_target(timeout_ms, target, method, ...)
    return shield.call_timeout(timeout_ms, target, method, ...)
end

return M
