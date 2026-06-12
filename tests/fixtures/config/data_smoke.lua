local M = {}

function M.on_init()
    for _ = 1, 3 do
        local ok_query, rows = shield.db.query("SELECT 1", {})
        assert(ok_query == true and type(rows) == "table", "db.query failed")

        local ok_exec, result = shield.db.execute("UPDATE t SET v = 1", {})
        assert(ok_exec == true and result.affected == 1, "db.execute failed")

        local ok_set = shield.redis.set("k", "v", 1)
        assert(ok_set == true, "redis.set failed")

        local ok_exists, exists = shield.redis.exists("k")
        assert(ok_exists == true and exists == false, "redis.exists failed")

        local ok_del, removed = shield.redis.del("k")
        assert(ok_del == true and removed == 0, "redis.del failed")
    end

    shield.exit("data_smoke_done")
end

return M
