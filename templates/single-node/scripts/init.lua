-- init.lua — Single-node game server entry point
-- Loaded at startup for all VMs in the pool.

log_info("=== Shield Single-Node Game Server ===")
log_info("Initializing game services...")

-- Global game state shared across all actors in this node
game_state = {
    players = {},
    rooms = {},
    started_at = get_current_time()
}

-- Utility: create a standard response
function ok(data)
    return { success = true, data = data or {} }
end

function fail(msg)
    return { success = false, error_message = msg or "error" }
end

log_info("Init script loaded.")
