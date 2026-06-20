-- Test service for on_error / on_panic hooks and on_exit call guard.

local M = {}

local last_error = nil
local last_error_context = nil
local last_panic = nil
local last_panic_context = nil
local call_in_exit_result = nil

function M.on_init(args)
    -- nothing special
end

function M.on_error(err, context)
    last_error = err
    last_error_context = context
end

function M.on_panic(reason, context)
    last_panic = reason
    last_panic_context = context
end

function M.on_exit(reason)
    -- Attempt shield.call during exit; should return false + api_not_allowed_in_exit.
    local ok, err = shield.call("any_target", "any_method")
    call_in_exit_result = { ok = ok, err = err }
end

-- A method that always throws.
function M.throwing_method()
    error("intentional_error")
end

-- A method that succeeds.
function M.good_method()
    return "ok"
end

function M.get_last_error()
    return last_error
end

function M.get_last_error_context()
    if last_error_context then
        return last_error_context.type, last_error_context.method
    end
    return nil, nil
end

function M.get_last_panic()
    return last_panic
end

function M.get_call_in_exit_result()
    if call_in_exit_result then
        return call_in_exit_result.ok, call_in_exit_result.err
    end
    return nil, nil
end

return M
