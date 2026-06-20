local M = {}

local legacy_on_message_called = false

function M.has_service_api()
    return shield.service ~= nil
end

function M.has_plugin_api()
    return shield.plugin ~= nil
end

function M.colon_db_call_fails()
    local ok = pcall(function()
        shield.db:query("SELECT 1", {})
    end)
    return not ok
end

function M.has_di_api()
    return shield.inject ~= nil or shield.container ~= nil
end

function M.on_message()
    legacy_on_message_called = true
end

function M.test_method()
    return "new_dispatch_only"
end

function M.on_message_called()
    return legacy_on_message_called
end

return M
