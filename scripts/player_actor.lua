-- Player Actor Lua Script
-- This script handles player-related game logic

-- Player state
local player_state = {
    player_id = nil,
    level = 1,
    experience = 0,
    gold = 100,
    health = 100,
    max_health = 100,
    position = {x = 0, y = 0, z = 0},
    online = true
}

-- Initialize player
function on_init()
    log_info("Player actor initialized")
    player_state.player_id = get_actor_id()
    log_info("Player ID: " .. player_state.player_id)
end

-- Handle incoming messages
function on_message(msg)
    log_info("Received message type: " .. msg.type .. " from " .. msg.sender_id)
    
    if msg.type == "get_info" then
        return handle_get_info(msg)
    elseif msg.type == "move" then
        return handle_move(msg)
    elseif msg.type == "attack" then
        return handle_attack(msg)
    elseif msg.type == "chat" then
        return handle_chat(msg)
    elseif msg.type == "gain_experience" then
        return handle_gain_experience(msg)
    elseif msg.type == "heal" then
        return handle_heal(msg)
    else
        log_error("Unknown message type: " .. msg.type)
        return create_response(false, {}, "Unknown message type")
    end
end

-- Get player information
function handle_get_info(msg)
    local response_data = {
        player_id = player_state.player_id,
        level = tostring(player_state.level),
        experience = tostring(player_state.experience),
        gold = tostring(player_state.gold),
        health = tostring(player_state.health),
        max_health = tostring(player_state.max_health),
        position_x = tostring(player_state.position.x),
        position_y = tostring(player_state.position.y),
        position_z = tostring(player_state.position.z),
        online = tostring(player_state.online)
    }
    
    log_info("Player info requested")
    return create_response(true, response_data)
end

-- Handle player movement
function handle_move(msg)
    local x = tonumber(msg.data.x or "0")
    local y = tonumber(msg.data.y or "0")
    local z = tonumber(msg.data.z or "0")
    
    -- Validate movement (simple bounds check)
    if x < -1000 or x > 1000 or y < -1000 or y > 1000 or z < -100 or z > 100 then
        return create_response(false, {}, "Invalid position coordinates")
    end
    
    player_state.position.x = x
    player_state.position.y = y
    player_state.position.z = z
    
    log_info("Player moved to position: " .. x .. ", " .. y .. ", " .. z)
    
    local response_data = {
        new_x = tostring(x),
        new_y = tostring(y),
        new_z = tostring(z),
        timestamp = tostring(get_current_time())
    }
    
    return create_response(true, response_data, "Movement successful")
end

-- Handle attack action
function handle_attack(msg)
    local target = msg.data.target or ""
    local damage = tonumber(msg.data.damage or "10")
    
    if target == "" then
        return create_response(false, {}, "No target specified")
    end
    
    -- Send attack message to target
    local attack_data = {
        attacker = player_state.player_id,
        damage = tostring(damage),
        timestamp = tostring(get_current_time())
    }
    
    send_message(target, "receive_damage", attack_data)
    
    log_info("Player attacked " .. target .. " for " .. damage .. " damage")
    
    local response_data = {
        target = target,
        damage_dealt = tostring(damage),
        success = "true"
    }
    
    return create_response(true, response_data, "Attack successful")
end

-- Handle chat message
function handle_chat(msg)
    local message = msg.data.message or ""
    local channel = msg.data.channel or "global"
    
    if message == "" then
        return create_response(false, {}, "Empty chat message")
    end
    
    log_info("Player chat [" .. channel .. "]: " .. message)
    
    -- In a real game, you would broadcast this to other players
    local response_data = {
        channel = channel,
        message = message,
        sender = player_state.player_id,
        timestamp = tostring(get_current_time())
    }
    
    return create_response(true, response_data, "Chat message sent")
end

-- Handle experience gain
function handle_gain_experience(msg)
    local exp_gain = tonumber(msg.data.experience or "0")
    
    if exp_gain <= 0 then
        return create_response(false, {}, "Invalid experience amount")
    end
    
    player_state.experience = player_state.experience + exp_gain
    
    -- Check for level up
    local exp_for_next_level = player_state.level * 100
    local leveled_up = false
    
    while player_state.experience >= exp_for_next_level do
        player_state.level = player_state.level + 1
        player_state.experience = player_state.experience - exp_for_next_level
        player_state.max_health = player_state.max_health + 10
        player_state.health = player_state.max_health -- Full heal on level up
        exp_for_next_level = player_state.level * 100
        leveled_up = true
    end
    
    local log_msg = "Player gained " .. exp_gain .. " experience"
    if leveled_up then
        log_msg = log_msg .. " and leveled up to level " .. player_state.level
    end
    log_info(log_msg)
    
    local response_data = {
        experience_gained = tostring(exp_gain),
        current_experience = tostring(player_state.experience),
        current_level = tostring(player_state.level),
        leveled_up = tostring(leveled_up),
        current_health = tostring(player_state.health),
        max_health = tostring(player_state.max_health)
    }
    
    return create_response(true, response_data, "Experience gained")
end

-- Handle healing
function handle_heal(msg)
    local heal_amount = tonumber(msg.data.amount or "20")
    
    if heal_amount <= 0 then
        return create_response(false, {}, "Invalid heal amount")
    end
    
    local old_health = player_state.health
    player_state.health = math.min(player_state.health + heal_amount, player_state.max_health)
    local actual_heal = player_state.health - old_health
    
    log_info("Player healed for " .. actual_heal .. " health")
    
    local response_data = {
        heal_amount = tostring(actual_heal),
        current_health = tostring(player_state.health),
        max_health = tostring(player_state.max_health)
    }
    
    return create_response(true, response_data, "Healing successful")
end
EOF < /dev/null