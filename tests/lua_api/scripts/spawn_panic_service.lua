-- Service exercising shield.panic from a handler. on_panic reports to a
-- watcher service via shield.send (fire-and-forget is allowed in hooks).

local M = {}

function M.on_init(args)
    local a = args.args or {}
    M.watcher = a.watcher
    return true
end

function M.on_panic(reason, context)
    if M.watcher then
        shield.send(M.watcher, "record_panic", reason,
                    context and context.type or "")
    end
end

function M.boom(ctx)
    shield.panic("test panic")
    return true
end

return M
