-- gateway_login.lua
-- Template for game login handling.
-- Receives: {"username":"...", "password":"..."}
-- Returns:  {"success":bool, "token":"...", "player_id":"..."}

local sessions = {}

function on_init()
    log_info("gateway_login service initialized")
end

function on_message(msg)
    if msg.type == "login" then
        return handle_login(msg)
    elseif msg.type == "verify" then
        return handle_verify(msg)
    else
        return { success = false, error_message = "Unknown type: " .. msg.type }
    end
end

function handle_login(msg)
    local username = msg.data.username or ""
    local password = msg.data.password or ""

    if username == "" or password == "" then
        return { success = false, error_message = "Missing credentials" }
    end

    -- Template: accept any login, generate a token
    local player_id = "player_" .. username
    local token = username .. "_" .. tostring(get_current_time())

    sessions[token] = {
        player_id = player_id,
        username = username,
        created = get_current_time()
    }

    log_info("Login success: " .. username)

    return {
        success = true,
        data = {
            token = token,
            player_id = player_id,
            username = username
        }
    }
end

function handle_verify(msg)
    local token = msg.data.token or ""
    local session = sessions[token]

    if not session then
        return { success = false, error_message = "Invalid token" }
    end

    return {
        success = true,
        data = {
            player_id = session.player_id,
            username = session.username
        }
    }
end
