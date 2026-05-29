-- shield_service.lua
-- Standard base class for all Shield Lua services.
-- Usage:
--   local service = require "shield_service"
--   local my_service = service.create_service { name = "my_service" }
--   my_service:register_handler("ping", function(self, msg)
--     return { success = true, data = { reply = "pong" } }
--   end)
--   return my_service

local M = {}

function M.create_service(config)
    config = config or {}
    local svc = {
        name = config.name or "unnamed",
        handlers = {},
    }

    function svc:on_init()
        if config.init then
            config.init(self)
        end
    end

    function svc:on_message(msg)
        local handler = self.handlers[msg.type]
        if handler then
            return handler(self, msg)
        end
        return {
            success = false,
            error_message = "Unknown message type: " .. (msg.type or "?"),
        }
    end

    function svc:register_handler(msg_type, handler)
        self.handlers[msg_type] = handler
    end

    -- Convenience wrappers around shield.* API (available at runtime)
    function svc:send(target, msg_type, data)
        return shield.send(target, msg_type, data or {})
    end

    function svc:call(target, msg_type, data, timeout_ms)
        return shield.call(target, msg_type, data or {}, timeout_ms)
    end

    function svc:query(name)
        return shield.query(name)
    end

    function svc:set_timeout(ms, callback)
        return shield.timeout(ms, callback)
    end

    function svc:list_services()
        return shield.list_services()
    end

    function svc:self_info()
        return shield.self()
    end

    function svc:node_id()
        return shield.node_id()
    end

    return svc
end

return M
