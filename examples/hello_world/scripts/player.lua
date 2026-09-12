-- player.lua - Hello World 玩家服务(认证后的 session target)
--
-- bind 成功后 ClientControlMessage::Bound 到达这里;move c2s RPC
-- (descriptor owner=player)在 spawn 期编译的绑定表中分发。
-- room/scene 属于玩家服务的私有转发决策:用 shield.send 把业务
-- 转给 room actor,回包由 room 经 s2c helper 发出。

local M = {
    clients = {},  -- session_id -> ClientContext(只读)
}

function M.on_init(args)
    M.name = args.name or "player"
    shield.log.info(M.name .. " started")
end

-- bind 成功后新 target 收到 Bound
function M.on_client_bound(ctx, client)
    M.clients[client:session_id()] = client
    shield.log.info("player online: " .. client:player_id())
    shield.send("room", "join", {
        client = client,
        player_id = client:player_id(),
    })
end

-- 认证后 c2s RPC(descriptor id=2, direction=c2s, requires_auth=true,
-- binding=move)。gateway 已在 descriptor 校验层拒绝过未认证请求,
-- 这里 assert 兜底:handler 收到的 client 身份是 gateway 附加的可信
-- 上下文,不是业务可伪造参数。
function M.move(ctx, client, request)
    assert(client:player_id() ~= "", "not authenticated")

    -- 目标服务转发给 room 由 Lua 内部决定:fire-and-forget,
    -- move_result 由 room 经 s2c helper 回发。
    shield.send("room", "move", {
        client = client,
        player_id = client:player_id(),
        x = tonumber(request.x) or 0,
        y = tonumber(request.y) or 0,
    })
end

-- 断开:session target 此时是 player
function M.on_disconnect(ctx, client, reason)
    M.clients[client:session_id()] = nil
    shield.log.info("player offline: " .. tostring(client:session_id()) ..
        " reason=" .. reason)
    shield.send("room", "leave", { session_id = client:session_id() })
end

-- 被踢下线(shield.client.close)
function M.on_client_unbound(ctx, client, reason)
    M.clients[client:session_id()] = nil
    shield.log.info("player kicked: " .. tostring(client:session_id()) ..
        " reason=" .. reason)
    shield.send("room", "leave", { session_id = client:session_id() })
end

function M.on_exit(reason)
    shield.log.info("player stopping: " .. reason)
end

return M
