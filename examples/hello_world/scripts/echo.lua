-- echo.lua - 最终 Lua API 形态 (spec)
-- 这个文件定义了 Shield 用户看到的 Lua API 契约

local echo = shield.service("echo")

function echo.on_init()
    shield.log.info("echo service started, id=" .. shield.self())
end

function echo.on_message(src, msg_type, data)
    if msg_type == "echo" then
        -- 原样返回
        shield.send(src, "echo_reply", data)
    elseif msg_type == "ping" then
        shield.send(src, "pong", { time = shield.now() })
    end
end

function echo.on_exit()
    shield.log.info("echo service stopping")
end
