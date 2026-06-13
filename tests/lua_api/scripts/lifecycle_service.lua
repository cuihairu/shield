-- Test service for LAPI-001 (Module Loader) and LAPI-002 (Lifecycle)

local M = {}

-- Test different on_init behaviors
function M.on_init(args)
    local test_case = args.config and args.config.test_case or "default"

    if test_case == "return_true" then
        return true
    elseif test_case == "return_false" then
        return false, "init_failed_intentionally"
    elseif test_case == "throw_error" then
        error("init_threw_error")
    end

    -- Default: no return value
end

function M.on_exit(reason)
    -- Can be tested with exit scenarios
end

-- Simple echo for basic testing
function M.echo(msg)
    return msg
end

-- Test method that returns multiple values
function M.multi_return()
    return "first", "second", "third"
end

-- Test method that returns error
function M.return_error()
    return false, "method_error"
end

-- Test method that throws
function M.throw_error()
    error("method_threw_error")
end

return M
