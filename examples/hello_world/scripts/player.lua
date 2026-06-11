-- player.lua - 用户参考示例

local M = {}

function M.on_init(args)
    M.session_id = args.args and args.args.session_id
    M.handle = shield.self()
    shield.log.info("player created: " .. tostring(M.handle))
end

function M.login(data)
    local src = shield.sender()
    local ok, rows = shield.db.query(
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
    if not M.user then
        return
    end

    shield.redis.publish("chat:" .. data.channel, {
        from = M.user.name,
        text = data.text,
    })
end

function M.logout()
    if M.user then
        shield.db.execute(
            "UPDATE users SET last_login = NOW() WHERE id = ?",
            { M.user.id }
        )
    end

    shield.exit("normal")
end

function M.on_exit(reason)
    shield.log.info("player exiting: " .. reason)
end

return M
