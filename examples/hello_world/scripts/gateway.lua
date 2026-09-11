-- gateway.lua - 用户参考示例
--
-- 单一 target 模型:每个 session 绑定唯一目标服务
-- 登录前:target = AuthService;shield.client.bind 后:target = PlayerService
-- Gateway 边界负责连接管理与单一 target 分发;入站业务经 spawn 期编译的
-- RPC 绑定以 handler(ctx, client, request) 到达目标服务
-- (完整 auth/player 闭环示例将在端到端里程碑补全)

local M = {
    sessions = {},
}

function M.on_init(args)
    M.name = args.name or "gateway"
    shield.log.info(M.name .. " started")
end

-- ClientControlMessage::Bound:客户端接入,初始绑定到本(auth 入口)服务
function M.on_client_bound(ctx, client)
    M.sessions[client:session_id()] = { connected = true }
    shield.log.info("client connected: " .. tostring(client:session_id()))
end

-- ClientControlMessage::Disconnected:客户端断开
function M.on_disconnect(ctx, client, reason)
    M.sessions[client:session_id()] = nil
    shield.log.info("client disconnected: " .. tostring(client:session_id())
        .. " reason=" .. reason)
end

-- ClientControlMessage::Unbound:被踢下线(shield.client.close)
function M.on_client_unbound(ctx, client, reason)
    M.sessions[client:session_id()] = nil
    shield.log.info("client unbound: " .. tostring(client:session_id())
        .. " reason=" .. reason)
end

return M
