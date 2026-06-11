-- gateway.lua - 用户参考示例

local M = {
    sessions = {},
}

function M.on_init(args)
    M.name = args.name or "gateway"
    shield.log.info(M.name .. " started")
end

function M.on_connect(session)
    local sid = session:id()
    local player, err = shield.spawn("player", {
        args = { session_id = sid },
    })

    if not player then
        shield.log.error("spawn player failed: " .. err.message)
        session:close("spawn_failed")
        return
    end

    M.sessions[sid] = {
        session = session,
        player = player,
    }
end

function M.on_client_message(session, payload)
    local sid = session:id()
    local entry = M.sessions[sid]
    if not entry then
        session:close("player_missing")
        return
    end

    if payload.type == "echo" then
        shield.send("echo", "echo", {
            session_id = sid,
            payload = payload.data,
        })
        return
    end

    if payload.type == "login" then
        shield.send(entry.player, "login", payload.data)
        return
    end

    if payload.type == "chat" then
        shield.send(entry.player, "chat", payload.data)
    end
end

function M.on_disconnect(session, reason)
    local sid = session:id()
    local entry = M.sessions[sid]
    if not entry then
        return
    end

    shield.send(entry.player, "logout", { reason = reason })
    M.sessions[sid] = nil
end

function M.echo_reply(data)
    local entry = M.sessions[data.session_id]
    if entry then
        entry.session:send({
            type = "echo_reply",
            data = data.payload,
        })
    end
end

function M.pong(data)
    local src = shield.sender()
    shield.log.debug("pong from " .. tostring(src) .. " at " .. tostring(data.time))
end

return M
