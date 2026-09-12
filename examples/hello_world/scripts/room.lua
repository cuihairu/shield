-- room.lua - Hello World 房间服务
--
-- 不直接持有 session:玩家服务把业务转发进来,房间经 s2c helper
-- (descriptor id=200, binding=move_result)向客户端回包。

local M = {
    members = {},               -- session_id -> {player_id, client}
    world = { width = 100, height = 100 },
}

function M.on_init(args)
    M.name = args.name or "room"
    shield.log.info(M.name .. " started (hall)")
end

function M.join(ctx, data)
    local session_id = data.client:session_id()
    M.members[session_id] = {
        player_id = data.player_id,
        client = data.client,
    }
    shield.log.info("room join: " .. tostring(data.player_id) ..
        " (" .. tostring(M.count()) .. " online)")
end

function M.move(ctx, data)
    local member = M.members[data.client:session_id()]
    if not member then
        return
    end
    -- 位置钳制到世界边界(演示业务逻辑),然后经 s2c helper 回包。
    local x = math.max(0, math.min(M.world.width, data.x))
    local y = math.max(0, math.min(M.world.height, data.y))
    shield.client_rpc.move_result(data.client, {
        player_id = member.player_id,
        x = x,
        y = y,
        online = M.count(),
    })
end

function M.leave(ctx, data)
    M.members[data.session_id] = nil
    shield.log.info("room leave: " .. tostring(M.count()) .. " online")
end

function M.count()
    local n = 0
    for _ in pairs(M.members) do
        n = n + 1
    end
    return n
end

function M.on_exit(reason)
    shield.log.info("room stopping: " .. reason)
end

return M
