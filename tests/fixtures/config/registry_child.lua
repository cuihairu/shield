local M = {}

function M.publish_alias(ctx, name)
    local ok, err = shield.register(name)
    assert(ok == true, err and err.message or "register failed")
end

function M.unpublish_alias(ctx, name)
    local ok, err = shield.unregister(name)
    assert(ok == true, err and err.message or "unregister failed")
end

function M.ping(ctx, value)
    return value .. ":" .. tostring(ctx.sender)
end

return M
