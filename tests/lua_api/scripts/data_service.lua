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

function M.test_db_transaction_commit()
    local ok, value = shield.db.transaction(function(tx)
        local ok_exec, result = tx.execute("UPDATE test SET value = ?", {1})
        if not ok_exec then
            return false, result
        end
        return true, result.affected
    end)
    return ok, value
end

function M.test_db_transaction_rollback()
    local ok, err = shield.db.transaction(function(tx)
        local ok_exec = tx.execute("UPDATE test SET value = ?", {2})
        if not ok_exec then
            return false, "execute failed"
        end
        return false, "rollback_requested"
    end)
    return ok, err
end

function M.test_db_transaction_closed_handle()
    local captured
    local ok = shield.db.transaction(function(tx)
        captured = tx
        return true
    end)
    if not ok then
        return false, "transaction failed"
    end

    local ok_after, err = captured.execute("UPDATE test SET value = ?", {3})
    return ok_after == false and err and err.code == "transaction_closed"
end

function M.test_db_mapper_select()
    local PlayerMapper = shield.db.mapper({
        SelectProfile = {
            type = "select",
            one = true,
            sql = "SELECT player_id, nickname FROM player WHERE player_id = #{player_id} AND shard = #{filter.shard}"
        }
    })

    local ok = PlayerMapper:SelectProfile({
        player_id = "p1",
        filter = { shard = 7 }
    })
    return ok
end

function M.test_db_mapper_transaction_required()
    shield.db.register_mapper("PlayerMapper", {
        DebitGold = {
            type = "update",
            transaction = "required",
            sql = "UPDATE wallet SET gold = gold - #{amount} WHERE player_id = #{player_id} AND gold >= #{amount}"
        }
    })

    local ok, result = shield.db.PlayerMapper:DebitGold({
        player_id = "p1",
        amount = 10
    })
    return ok, result and result.affected
end

function M.test_db_mapper_reuses_transaction()
    local PlayerMapper = shield.db.mapper({
        UpdateName = {
            type = "update",
            sql = "UPDATE player SET nickname = #{nickname} WHERE player_id = #{player_id}"
        }
    })

    local ok, affected = shield.db.transaction(function(tx)
        local ok_update, result = PlayerMapper:UpdateName(tx, {
            player_id = "p1",
            nickname = "neo"
        })
        if not ok_update then
            return false, result
        end
        return true, result.affected
    end)
    return ok, affected
end

function M.test_db_mapper_rejects_raw_substitution()
    local UnsafeMapper = shield.db.mapper({
        List = {
            type = "select",
            sql = "SELECT * FROM player ORDER BY ${order_by}"
        }
    })

    local ok, err = UnsafeMapper:List({ order_by = "level" })
    return ok == false and err and err.code == "mapper_unsafe_sql"
end

function M.test_db_entity_insert()
    local Player = shield.db.entity({
        table = "player",
        fields = { "player_id", "nickname", "level" },
        primary_key = "player_id"
    })

    local ok, result = Player:insert({
        player_id = "p1",
        nickname = "neo",
        level = 9
    })
    return ok, result and result.affected
end

function M.test_db_entity_update()
    local Player = shield.db.entity({
        table = "player",
        fields = { "player_id", "nickname", "level" },
        primary_key = "player_id"
    })

    local ok, result = Player:update({
        player_id = "p1",
        nickname = "trinity",
        level = 10
    })
    return ok, result and result.affected
end

function M.test_db_entity_find()
    local Player = shield.db.entity({
        table = "player",
        fields = { "player_id", "nickname", "level" },
        primary_key = "player_id"
    })

    local ok = Player:find("p1")
    return ok
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
