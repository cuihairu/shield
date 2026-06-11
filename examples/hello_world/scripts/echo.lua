-- echo.lua - 用户参考示例

local M = {}

function M.on_init(args)
    M.name = args.name or "echo"
    shield.log.info(M.name .. " started")
end

function M.echo(data)
    local src = shield.sender()
    shield.send(src, "echo_reply", data)
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

function M.on_exit(reason)
    shield.log.info("echo stopping: " .. reason)
end

return M
