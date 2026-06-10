-- player.lua - 玩家 Actor (最终 API 形态)
-- 每个在线玩家一个 actor 实例

local player = shield.service("player")

function player.on_init(args)
    player.session = args.session
    player.id = shield.self()
    shield.log.info("player actor created: " .. player.id)
end

function player.on_message(src, msg_type, data)
    if msg_type == "login" then
        -- 原始 SQL 查询，无 ORM
        local rows = shield.db:query("SELECT * FROM users WHERE id = ?", { data.user_id })
        if #rows > 0 then
            player.user = rows[1]
            shield.send(src, "login_ok", { name = player.user.name })
        end

    elseif msg_type == "chat" then
        -- 原始 Redis 操作
        shield.redis:publish("chat:" .. data.channel, {
            from = player.user.name,
            text = data.text
        })

    elseif msg_type == "logout" then
        shield.db:execute("UPDATE users SET last_login = NOW() WHERE id = ?", { player.user.id })
        shield.exit()
    end
end

function player.on_timer(timer_id)
    if timer_id == "heartbeat_timeout" then
        shield.log.warn("player heartbeat timeout: " .. player.id)
        shield.send("gateway", "kick", { session = player.session })
    end
end
