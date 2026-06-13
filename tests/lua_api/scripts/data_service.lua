-- Test service for LAPI-008 (Data API)

local M = {}

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

function M.test_db_query()
    local ok, rows = shield.db.query("SELECT * FROM test WHERE id = ?", {1})
    return ok, rows
end

function M.test_db_execute()
    local ok, affected = shield.db.execute("INSERT INTO test (name) VALUES (?)", {"test"})
    return ok, affected
end

function M.test_redis_get()
    local ok, value = shield.redis.get("test_key")
    return ok, value
end

function M.test_redis_set()
    local ok = shield.redis.set("test_key", "test_value")
    return ok
end

function M.test_redis_del()
    local ok = shield.redis.del("test_key")
    return ok
end

function M.test_redis_subscribe()
    local ok = shield.redis.subscribe("test_channel", function(channel, message)
        -- Handle pub/sub message
    end)
    return ok
end

return M
