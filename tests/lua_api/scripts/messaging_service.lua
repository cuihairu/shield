-- Test service for LAPI-003 (Registry), LAPI-004 (Message Send), LAPI-005 (Message Call)

local M = {}

local last_sender = nil
local last_method = nil
local last_args = nil

function M.on_init(args)
    -- Register additional name if config requests
    if args.config and args.config.register_alias then
        shield.register(args.config.register_alias)
    end
end

function M.echo(msg)
    return msg
end

function M.get_sender()
    return last_sender
end

function M.get_last_call()
    return last_method, last_args
end

function M.call_handler(method, ...)
    last_method = method
    last_args = {...}

    if method == "return_value" then
        return "returned_value"
    elseif method == "return_false" then
        return false, "return_false_reason"
    elseif method == "return_nil" then
        return nil
    elseif method == "throw_error" then
        error("handler_error")
    elseif method == "multi_return" then
        return "first", "second", "third"
    end

    return "default_response"
end

-- Generic handler for send/call
function M.handle_message(src, method, args)
    last_sender = src
    last_method = method
    last_args = args

    if method == "echo" then
        return args[1] or args
    elseif method == "return_error" then
        return false, "handler_error"
    elseif method == "throw" then
        error("handler_threw_error")
    end

    return "ok"
end

function M.check_sender()
    local sender = shield.sender()
    return sender ~= nil
end

return M
