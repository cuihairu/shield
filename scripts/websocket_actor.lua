-- WebSocket Actor Lua Script
-- This script handles real-time WebSocket connections

local connection_state = {
    connected_at = nil,
    message_count = 0,
    last_ping = nil
}

function on_init()
    log_info("WebSocket actor initialized")
    connection_state.connected_at = get_current_time()
    log_info("Connection established at: " .. connection_state.connected_at)
end

function on_message(msg)
    connection_state.message_count = connection_state.message_count + 1
    log_info("WebSocket message #" .. connection_state.message_count .. " of type: " .. msg.type)
    
    if msg.type == "ping" then
        return handle_ping(msg)
    elseif msg.type == "join_room" then
        return handle_join_room(msg)
    elseif msg.type == "chat_message" then
        return handle_chat_message(msg)
    elseif msg.type == "game_update" then
        return handle_game_update(msg)
    elseif msg.type == "heartbeat" then
        return handle_heartbeat(msg)
    else
        log_info("Unknown WebSocket message type: " .. msg.type)
        return create_response(true, {
            type = "echo",
            original_type = msg.type,
            message = "Message received and echoed back"
        }, "Echo response")
    end
end

function handle_ping(msg)
    connection_state.last_ping = get_current_time()
    
    local response_data = {
        type = "pong",
        timestamp = tostring(connection_state.last_ping),
        client_timestamp = msg.data.timestamp or "unknown"
    }
    
    return create_response(true, response_data, "Pong response")
end

function handle_join_room(msg)
    local room_id = msg.data.room_id or ""
    local player_id = msg.data.player_id or ""
    
    if room_id == "" or player_id == "" then
        return create_response(false, {
            type = "join_room_error",
            error = "Missing room_id or player_id"
        }, "Join room failed")
    end
    
    log_info("Player " .. player_id .. " joining room: " .. room_id)
    
    local response_data = {
        type = "room_joined",
        room_id = room_id,
        player_id = player_id,
        success = "true",
        room_info = {
            name = "Room " .. room_id,
            player_count = "5",
            max_players = "10"
        }
    }
    
    return create_response(true, response_data, "Successfully joined room")
end

function handle_chat_message(msg)
    local message = msg.data.message or ""
    local channel = msg.data.channel or "general"
    local sender = msg.data.sender or "anonymous"
    
    if message == "" then
        return create_response(false, {
            type = "chat_error",
            error = "Empty message"
        }, "Chat message failed")
    end
    
    log_info("Chat message from " .. sender .. " in " .. channel .. ": " .. message)
    
    -- In a real system, you would broadcast this to other players in the channel
    local response_data = {
        type = "chat_broadcast",
        sender = sender,
        channel = channel,
        message = message,
        timestamp = tostring(get_current_time()),
        broadcast_id = "msg_" .. connection_state.message_count
    }
    
    return create_response(true, response_data, "Chat message broadcast")
end

function handle_game_update(msg)
    local update_type = msg.data.update_type or ""
    local data = msg.data
    
    log_info("Game update received: " .. update_type)
    
    local response_data = {
        type = "update_received",
        update_type = update_type,
        processed_at = tostring(get_current_time()),
        ack = "true"
    }
    
    -- Add update-specific responses
    if update_type == "position" then
        response_data.new_position = {
            x = data.x or "0",
            y = data.y or "0",
            z = data.z or "0"
        }
    elseif update_type == "state" then
        response_data.state_confirmed = data.state or "unknown"
    end
    
    return create_response(true, response_data, "Game update processed")
end

function handle_heartbeat(msg)
    local client_time = msg.data.client_time or "0"
    local current_time = get_current_time()
    
    local response_data = {
        type = "heartbeat_ack",
        server_time = tostring(current_time),
        client_time = client_time,
        round_trip_start = client_time,
        connection_duration = tostring(current_time - connection_state.connected_at),
        message_count = tostring(connection_state.message_count)
    }
    
    return create_response(true, response_data, "Heartbeat acknowledged")
end