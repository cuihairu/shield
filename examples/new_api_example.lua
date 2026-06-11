-- New Shield Lua API Example
-- This demonstrates the refactored Lua service interface

local M = {}

-- Called when service is created
function M.on_init(args)
    M.name = args.name or "unnamed"
    M.config = args.config or {}
    M.count = 0

    shield.log.info(M.name .. " started")

    -- Start a heartbeat timer
    M.heartbeat_timer = shield.timer(5000, function()
        shield.log.debug("heartbeat: " .. M.name)
    end)
end

-- A simple ping method
function M.ping(value)
    local src = shield.sender()
    shield.log.info("ping from " .. tostring(src))

    -- Send pong back to sender
    if src then
        shield.send(src, "pong", value or "hello")
    end

    return "pong:" .. (value or "hello")
end

-- A method that calls another service
function M.get_player_info(player_id)
    -- Call another service
    local ok, result = shield.call("player_db", "get", {
        id = player_id
    })

    if ok then
        return result
    else
        return nil, "player not found"
    end
end

-- Async task example
function M.async_task()
    shield.fork(function()
        shield.sleep(1000)
        shield.log.info("async task completed")

        -- Send a message when done
        shield.send("worker", "task_done", { task = "example" })
    end)
end

-- Called when service is stopping
function M.on_exit(reason)
    shield.log.info(M.name .. " exiting: " .. reason)

    -- Cancel owned timers
    if M.heartbeat_timer then
        shield.cancel_timer(M.heartbeat_timer)
    end
end

-- Error handler
function M.on_error(err, context)
    shield.log.error(context.type .. ": " .. tostring(err))
end

-- Panic handler (best-effort only)
function M.on_panic(reason, context)
    shield.log.error("panic: " .. reason)
    -- Cannot call shield.call or shield.sleep here
end

return M
