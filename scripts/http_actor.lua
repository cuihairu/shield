-- HTTP Actor Lua Script
-- This script handles HTTP requests through the gateway

local request_count = 0

function on_init()
    log_info("HTTP actor initialized")
    log_info("Actor ID: " .. get_actor_id())
end

function on_message(msg)
    request_count = request_count + 1
    log_info("Processing HTTP request #" .. request_count .. " of type: " .. msg.type)
    
    if msg.type == "game_action" then
        return handle_game_action(msg)
    elseif msg.type == "status" then
        return handle_status(msg)
    elseif msg.type == "player_stats" then
        return handle_player_stats(msg)
    else
        return create_response(false, {}, "Unknown HTTP request type")
    end
end

function handle_game_action(msg)
    local action = msg.data.action or ""
    local player_id = msg.data.player_id or ""
    
    if action == "" or player_id == "" then
        return create_response(false, {error = "Missing action or player_id"}, "Bad request")
    end
    
    log_info("Processing game action: " .. action .. " for player: " .. player_id)
    
    -- Simulate some game logic processing
    local response_data = {
        action = action,
        player_id = player_id,
        result = "success",
        timestamp = tostring(get_current_time()),
        server_response = "Game action processed successfully"
    }
    
    return create_response(true, response_data, "Game action completed")
end

function handle_status(msg)
    local response_data = {
        server_status = "online",
        request_count = tostring(request_count),
        uptime = tostring(get_current_time()),
        actor_id = get_actor_id()
    }
    
    return create_response(true, response_data, "Server status retrieved")
end

function handle_player_stats(msg)
    local player_id = msg.data.player_id or ""
    
    if player_id == "" then
        return create_response(false, {error = "Missing player_id"}, "Bad request")
    end
    
    -- Simulate fetching player stats
    local response_data = {
        player_id = player_id,
        level = "10",
        experience = "2500",
        gold = "1500",
        last_login = tostring(get_current_time()),
        status = "online"
    }
    
    return create_response(true, response_data, "Player stats retrieved")
end