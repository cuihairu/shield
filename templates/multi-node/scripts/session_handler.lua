-- session_handler.lua — Multi-node WebSocket session handler
-- Gateway-level session management. Forwards game messages to logic nodes.

local sessions = {}

function on_init()
    log_info("Multi-node session handler initialized")
end

function on_message(msg)
    local msg_type = msg.type or ""

    if msg_type == "ping" then
        return ok({ type = "pong" })
    end

    if msg_type == "login" then
        local username = msg.data and msg.data.username or ""
        if username == "" then
            return fail("Missing username")
        end
        local player_id = "player_" .. username
        sessions[player_id] = { username = username }
        log_info("Player logged in via gateway: " .. username)
        return ok({ player_id = player_id })
    end

    -- Forward game messages to logic node
    if shield and shield.call then
        local result = route_to_logic(
            msg.data and msg.data.player_id or "0",
            msg_type,
            msg.data or {}
        )
        return result or ok({ forwarded = true })
    end

    return ok({ echo = msg_type })
end
