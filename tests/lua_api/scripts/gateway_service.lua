-- Test service for LAPI-009 (Gateway API)
--
-- Gateway lifecycle handlers under the single-target client identity:
--   on_connect(client_context)
--   on_disconnect(client_context, reason)
--   on_client_message(route_id, client_context, body, message)
-- The dispatch prepends the call context table as the first argument, so the
-- Lua signatures below carry the leading ctx. client_context materializes as
-- a read-only ClientContext userdata when the payload carries the
-- __shield_client_ref marker (real bridge path); the direct-call tests pass
-- plain tables, so both shapes are handled.

local M = {}

local sessions = {}

-- Identity key for a ClientContext userdata or a plain context table.
local function session_key(client)
    if type(client) == "userdata" then
        local ok, id = pcall(client.session_id, client)
        if ok then
            return tostring(id)
        end
    elseif type(client) == "table" then
        local id = client.session_id or client.id
        if id ~= nil then
            return tostring(id)
        end
    end
    return nil
end

local function player_of(client)
    if type(client) == "userdata" then
        local ok, player = pcall(client.player_id, client)
        if ok then
            return player
        end
    elseif type(client) == "table" then
        return client.player_id
    end
    return nil
end

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

function M.get_sessions(ctx)
    return sessions
end

function M.on_connect(ctx, client)
    local key = session_key(client)
    if key == nil then
        return false
    end
    sessions[key] = {
        id = key,
        connected = true,
        player_id = player_of(client),
        connect_time = shield.now()
    }
    return true
end

function M.on_disconnect(ctx, client, reason)
    local key = session_key(client)
    if key and sessions[key] then
        sessions[key].connected = false
        sessions[key].disconnect_reason = reason
        sessions[key].disconnect_time = shield.now()
    end
end

function M.on_client_message(ctx, route_id, client, body, message)
    local key = session_key(client)
    if key and sessions[key] then
        sessions[key].last_message = {
            route_id = route_id,
            body = body,
            message = message,
            player_id = player_of(client),
            time = shield.now()
        }
    end
    return true
end

return M
