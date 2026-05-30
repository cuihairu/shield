-- session_handler.lua — WebSocket session handler for single-node template
-- Handles: connect, heartbeat, disconnect, and custom messages.

local sessions = {}

function on_init()
    log_info("Session handler initialized")
end

function on_message(msg)
    local msg_type = msg.type or ""

    if msg_type == "ping" or msg_type == "heartbeat" then
        return ok({ type = "pong", ts = tostring(get_current_time()) })
    end

    if msg_type == "login" then
        local username = msg.data and msg.data.username or ""
        if username == "" then
            return fail("Missing username")
        end
        local player_id = "player_" .. username
        sessions[player_id] = { username = username, connected = true }
        log_info("Player logged in: " .. username)
        return ok({ player_id = player_id, token = username .. "_token" })
    end

    if msg_type == "chat" then
        local text = msg.data and msg.data.text or ""
        log_info("Chat: " .. text)
        return ok({ echoed = text })
    end

    return fail("Unknown message type: " .. msg_type)
end
