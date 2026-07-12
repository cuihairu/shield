-- gateway.lua - 用户参考示例
--
-- 新模型：所有客户端消息通过 session.target 路由
-- 登录前：session.target = AuthService
-- 登录后：session.target = PlayerService
--
-- Gateway 只做连接管理和消息转发，不做业务分发。
-- route_id 在 wire header 中，body 是纯业务数据。

local M = {
    sessions = {},
}

function M.on_init(args)
    M.name = args.name or "gateway"
    shield.log.info(M.name .. " started")
end

function M.on_connect(session)
    local sid = session:id()
    M.sessions[sid] = {
        session = session,
    }
    shield.log.info("client connected: " .. tostring(sid))
end

-- 新签名：on_client_message(route_id, client_context, body)
-- route_id: 来自 wire header
-- client_context: {session_id, session_epoch, player_id, gateway_service}
-- body: 原始 body 字节（目标 VM 按 RPC schema 解码）
function M.on_client_message(route_id, client_context, body)
    local sid = client_context.session_id
    local entry = M.sessions[tostring(sid)]
    if not entry then
        shield.log.warn("no session for: " .. tostring(sid))
        return
    end

    -- Gateway 不做业务分发，直接转发给目标服务
    -- 目标服务由 session.target 决定（在 C++ 层设置）
    shield.log.info("route_id=" .. tostring(route_id) .. " from session " .. tostring(sid))
end

function M.on_disconnect(session, reason)
    local sid = session:id()
    local entry = M.sessions[sid]
    if not entry then
        return
    end

    -- 通知当前目标服务断线
    local target = entry.target_service
    if target then
        shield.send(target, "on_client_disconnect", {
            session_id = sid,
            reason = reason,
        })
    end

    M.sessions[sid] = nil
    shield.log.info("client disconnected: " .. tostring(sid) .. " reason=" .. reason)
end

return M
