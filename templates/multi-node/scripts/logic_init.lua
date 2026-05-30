-- logic_init.lua — Logic node initialization
-- Registers game services that gateway nodes can call.

log_info("=== Shield Logic Node ===")

-- Game state for this logic node
local game = {
    players = {},
    rooms = {}
}

log_info("Logic init complete. Ready to process game messages.")
