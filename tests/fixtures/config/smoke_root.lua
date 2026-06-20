local M = {}

function M.on_init(args)
    assert(shield.self():id() == args.name, "self must match service name")

    local now = shield.now()
    assert(type(now) == "number" and now > 0, "now must return milliseconds")

    local child, spawn_err = shield.spawn("smoke_child", {
        name = "smoke_child.1",
        args = { greeting = "hello" },
    })
    assert(child, spawn_err and spawn_err.message or "spawn failed")

    local ok, reply, sender = shield.call(child, "echo", "ping")
    assert(ok, reply and reply.message or "call failed")
    assert(reply == "hello:ping", "unexpected call reply")
    assert(sender == args.name, "sender must be caller service")

    local sent = shield.call(child, "mark", args.name)
    assert(sent == true, "send to child failed")

    local ok_mark, marked = shield.call(child, "get_marked")
    assert(ok_mark and marked == args.name, "send handler did not run")

    local names = shield.names()
    assert(#names >= 1, "expected child service")

    shield.exit("smoke_done")
end

function M.on_exit(reason)
    assert(reason == "smoke_done" or reason == "stopping")
end

return M
