-- player.lua - 用户参考示例
--
-- 演示新版插件自治 Lua API：
--   shield.database.<driver>(binding) 返回绑定到该逻辑名的 proxy
--   shield.cache.redis(binding)       同理
-- binding 由 app.yaml 的 plugins.bindings 声明；未声明时 proxy 为 nil。

local M = {}

-- 通过 binding 逻辑名拿到数据库 / 缓存 proxy。
-- app.yaml 中需要声明：
--   plugins:
--     instances:
--       - { id: "db.main",    package: "database.sqlite", required: true,
--           config: { database: "data/game.db" } }
--       - { id: "cache.chat", package: "cache.redis",     required: true,
--           config: { host: "127.0.0.1", port: 6379 } }
--     bindings:
--       database.default: "db.main"
--       cache.chat: "cache.chat"
-- 未配置时 shield.database.sqlite / shield.cache.redis 返回 nil，业务自行降级。
local DB    = shield.database.sqlite("database.default")
local Cache = shield.cache.redis("cache.chat")

function M.on_init(args)
    M.session_id = args.args and args.args.session_id
    M.handle = shield.self()
    shield.log.info("player created: " .. tostring(M.handle))
end

function M.login(data)
    local src = shield.sender()

    if not DB then
        shield.send(src, "login_failed", { message = "db_unavailable" })
        return
    end

    local ok, rows = DB:query(
        "SELECT * FROM users WHERE id = ?",
        { data.user_id }
    )
    if not ok then
        shield.send(src, "login_failed", { message = "db_error" })
        return
    end

    if #rows == 0 then
        shield.send(src, "login_failed", { message = "user_not_found" })
        return
    end

    M.user = rows[1]
    shield.send(src, "login_ok", { name = M.user.name })
end

function M.chat(data)
    if not M.user or not Cache then
        return
    end

    -- cache.redis 插件的 publish 对应 Redis PUBLISH，返回订阅者数。
    Cache:publish("chat:" .. data.channel, {
        from = M.user.name,
        text = data.text,
    })
end

function M.logout()
    if M.user and DB then
        DB:execute(
            "UPDATE users SET last_login = datetime('now') WHERE id = ?",
            { M.user.id }
        )
    end

    shield.exit("normal")
end

function M.on_exit(reason)
    shield.log.info("player exiting: " .. reason)
end

return M
