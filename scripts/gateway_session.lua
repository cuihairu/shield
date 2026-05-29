-- gateway_session.lua
-- Template for WebSocket session management.
-- Handles: connect, heartbeat, disconnect, and custom messages.

local connections = {}

function on_init()
    log_info("gateway_session service initialized")
end

function on_message(msg)
    local conn_id = msg.data.connection_id or msg.sender_id or ""

    if msg.type == "ws_connect" then
        return handle_connect(conn_id, msg)
    elseif msg.type == "ws_disconnect" then
        return handle_disconnect(conn_id)
    elseif msg.type == "heartbeat" or msg.type == "ping" then
        return handle_heartbeat(conn_id)
    else
        return handle_custom(conn_id, msg)
    end
end

function handle_connect(conn_id, msg)
    local token = msg.data.token or ""

    connections[conn_id] = {
        token = token,
        connected_at = get_current_time(),
        last_heartbeat = get_current_time()
    }

    log_info("Session connected: " .. conn_id)

    return {
        success = true,
        data = {
            session_id = conn_id,
            status = "connected"
        }
    }
end

function handle_disconnect(conn_id)
    connections[conn_id] = nil
    log_info("Session disconnected: " .. conn_id)

    return {
        success = true,
        data = { status = "disconnected" }
    }
end

function handle_heartbeat(conn_id)
    local conn = connections[conn_id]
    if conn then
        conn.last_heartbeat = get_current_time()
    end

    return {
        success = true,
        data = {
            type = "pong",
            timestamp = tostring(get_current_time())
        }
    }
end

function handle_custom(conn_id, msg)
    local conn = connections[conn_id]
    if not conn then
        return { success = false, error_message = "Not authenticated" }
    end

    -- Route to business logic
    return {
        success = true,
        data = {
            echo_type = msg.type,
            session_id = conn_id
        }
    }
end
