-- gateway_init.lua — Gateway node initialization
-- Sets up routing to logic nodes.

log_info("=== Shield Gateway Node ===")

-- In a multi-node setup, the gateway forwards messages to logic nodes.
-- Use shield.send / shield.call to route to named logic services.

function route_to_logic(player_id, msg_type, data)
    -- Hash player to a logic node for session affinity
    local node = "logic-" .. tostring(math.fmod(tonumber(player_id) or 0, 2) + 1)
    return shield.call(node .. "_dispatcher", msg_type, data)
end

log_info("Gateway init complete. Ready to route to logic nodes.")
