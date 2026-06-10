-- gateway.lua - 网关服务 (最终 API 形态)
-- Gateway 的业务路由层是 Lua 脚本，不是框架代码

local gateway = shield.service("gateway")

function gateway.on_init()
    shield.log.info("gateway listening on tcp://0.0.0.0:8001")
end

-- 新连接接入
function gateway.on_connect(session_id)
    shield.log.info("session connected: " .. session_id)
    -- 为新连接创建 player actor
    shield.spawn("player", { session = session_id })
end

-- 收到客户端消息
function gateway.on_session_message(session_id, msg_type, data)
    if msg_type == "echo" then
        -- 路由到 echo 服务
        shield.send("echo", "echo", {
            session = session_id,
            payload = data
        })
    elseif msg_type == "login" then
        -- 路由到对应 player
        local player = gateway.sessions[session_id]
        if player then
            shield.send(player, "login", data)
        end
    end
end

-- 连接断开
function gateway.on_disconnect(session_id)
    shield.log.info("session disconnected: " .. session_id)
    local player = gateway.sessions[session_id]
    if player then
        shield.send(player, "logout", {})
        gateway.sessions[session_id] = nil
    end
end

-- echo 服务的回复，转发回客户端
function gateway.on_message(src, msg_type, data)
    if msg_type == "echo_reply" then
        gateway.send_to_session(data.session, "echo_reply", data.payload)
    end
end
