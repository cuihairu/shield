-- auth.lua - Hello World 认证入口(listener 的 auth 入口服务)
--
-- session 建立后初始 target = auth:预登录 c2s RPC 在此分发。
-- shield.client.bind 成功后 target 原子切换到 "player",之后
-- move 等 c2s RPC 直接由 player 的启动期绑定表承接。

local M = {
    connected = 0,
}

function M.on_init(args)
    M.name = args.name or "auth"
    shield.log.info(M.name .. " started (auth entry)")
end

-- ClientControlMessage::Bound:连接建立,尚未认证
function M.on_client_bound(ctx, client)
    M.connected = M.connected + 1
    shield.log.info("client connected: " .. tostring(client:session_id()))
end

-- 预登录 c2s RPC(descriptor id=1, requires_auth=false, binding=login)。
-- handler 形态固定为 handler(ctx, client, request);入站是
-- fire-and-forget,响应必须经 s2c helper 显式发出。
function M.login(ctx, client, request)
    local player_id = tostring(request.player_id or "")
    if player_id == "" then
        shield.log.warn("login rejected: missing player_id")
        return
    end

    -- 挂起当前协程;gateway CAS 成功后恢复并拿到 ClientRef,
    -- session target 已原子切换到 "player"(epoch 递增)。
    local ok, ref = shield.client.bind(client, player_id, "player")
    if not ok then
        shield.log.warn("bind failed: " .. tostring(ref.code or ref))
        return
    end

    -- 登录结果经 s2c helper(descriptor id=100, binding=login_result)回包
    shield.client_rpc.login_result(ref, {
        player_id = ref:player_id(),
        session_id = ref:session_id(),
        room_hint = "hall",
    })
end

-- 认证前断开:session 仍绑定在 auth 上
function M.on_disconnect(ctx, client, reason)
    M.connected = math.max(0, M.connected - 1)
    shield.log.info("client left before auth: " ..
        tostring(client:session_id()) .. " reason=" .. reason)
end

function M.on_exit(reason)
    shield.log.info("auth stopping: " .. reason)
end

return M
