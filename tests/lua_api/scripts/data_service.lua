-- Test service for LAPI-008 (Data API)
--
-- Exercises shield.db.* and shield.redis.* from Lua. The test harness
-- initialises the global mock pools so these calls go through the real
-- binding layer without requiring a live database or Redis server.

local M = {}

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

-- DB API ---------------------------------------------------------------

function M.test_db_query()
    local ok, rows = shield.db.query("SELECT * FROM test WHERE id = ?", {1})
    return ok, rows
end

function M.test_db_execute()
    local ok, result = shield.db.execute("INSERT INTO test (name) VALUES (?)", {"test"})
    return ok, result
end

-- Redis API ------------------------------------------------------------

function M.test_redis_get()
    local ok, value = shield.redis.get("test_key")
    return ok, value
end

function M.test_redis_set()
    local ok, err = shield.redis.set("test_key", "test_value")
    return ok, err
end

function M.test_redis_del()
    local ok, count = shield.redis.del("test_key")
    return ok, count
end

function M.test_redis_exists()
    local ok, exists = shield.redis.exists("test_key")
    return ok, exists
end

-- Dot notation test: shield.db:query() should fail (colon syntax) -----

function M.test_colon_db_fails()
    local ok = pcall(function()
        shield.db:query("SELECT 1", {})
    end)
    return not ok
end

return M
