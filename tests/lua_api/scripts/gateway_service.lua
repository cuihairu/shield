-- Test service for LAPI-009 (Gateway API)
--
-- Handles on_connect / on_disconnect / on_client_message gateway pattern.
-- Session objects may be full userdata (MockSessionHandle) or plain tables
-- depending on the test harness.

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

    -- Extract id from session (userdata or table).
    if type(session) == "table" then
        session_id = session.id or session_id
    elseif type(session) == "userdata" and session.id then
        session_id = session:id() or session_id
    end

    local addr = "unknown"
    if type(session) == "table" then
        addr = session.remote_addr or addr
    elseif type(session) == "userdata" and session.remote_addr then
        addr = session:remote_addr() or addr
    end

    sessions[session_id] = {
        id = session_id,
        connected = true,
        remote_address = addr,
        connect_time = shield.now()
    }

    -- Store session reference for later use.
    if type(session) == "table" then
        session._internal_id = session_id
    end

    return true
end

function M.on_disconnect(session, reason)
    local session_id = nil
    if type(session) == "table" then
        session_id = session.id or session._internal_id
    elseif type(session) == "userdata" and session.id then
        session_id = session:id()
    end
    if session_id and sessions[session_id] then
        sessions[session_id].connected = false
        sessions[session_id].disconnect_reason = reason
        sessions[session_id].disconnect_time = shield.now()
    end
end

function M.on_client_message(session, payload)
    local session_id = nil
    if type(session) == "table" then
        session_id = session.id or session._internal_id
    elseif type(session) == "userdata" and session.id then
        session_id = session:id()
    end

    if session_id and sessions[session_id] then
        sessions[session_id].last_message = {
            payload = payload,
            time = shield.now()
        }
    end

    -- Echo back via session:send if available.
    if type(session) == "userdata" and session.send then
        local ok, err = session:send(payload)
        if session_id and sessions[session_id] then
            sessions[session_id].last_send = {
                ok = ok,
                error = err,
                time = shield.now()
            }
        end
    end

    return true
end

return M
