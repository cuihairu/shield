-- Test service for LAPI-009 (Gateway API)

local M = {}

local sessions = {}
local session_counter = 0

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

function M.get_sessions()
    return sessions
end

function M.on_connect(session)
    session_counter = session_counter + 1
    local session_id = "session_" .. session_counter

    sessions[session_id] = {
        id = session_id,
        connected = true,
        remote_address = session:remote_address() or "unknown",
        connect_time = shield.now()
    }

    -- Store session reference
    session._id = session_id

    return true
end

function M.on_disconnect(session, reason)
    local session_id = session._id
    if session_id and sessions[session_id] then
        sessions[session_id].connected = false
        sessions[session_id].disconnect_reason = reason
        sessions[session_id].disconnect_time = shield.now()
    end
end

function M.on_client_message(session, payload)
    local session_id = session._id or "unknown"
    if sessions[session_id] then
        sessions[session_id].last_message = {
            payload = payload,
            time = shield.now()
        }
    end

    -- Echo back the payload
    session:send(payload)
end

function M.send_large_message(session)
    -- Test send queue behavior
    local large_payload = string.rep("x", 1000000)  -- 1MB
    local ok, err = session:send(large_payload)
    return ok, err
end

function M.send_after_close(session_id)
    local session = sessions[session_id]
    if not session then
        return false, "session_not_found"
    end

    -- Simulate sending after session closed
    session.connected = false
    -- Try to send
    -- session:send("test")  -- This would fail
    return false, "session_closed"
end

return M
