-- Test service for LAPI-009 (Gateway API)
--
-- Single-target client handling under the M3 typed-dispatch contract:
--   on_client_bound(ctx, client)                     -- ClientControlMessage::Bound
--   on_disconnect(ctx, client, reason)               -- ClientControlMessage::Disconnected
--   on_client_unbound(ctx, client, reason)           -- ClientControlMessage::Unbound (kick)
--   <binding>(ctx, client, request)                  -- compiled c2s/bidi RPC handlers
-- The dispatch prepends the call context table as the first argument, so the
-- Lua signatures below carry the leading ctx. `client` materializes as a
-- read-only ClientContext userdata on the bridge path; the direct-call tests
-- pass plain tables, so both shapes are handled. `request` is the descriptor
-- contract value: decoded_request table when the pipeline codec produced one,
-- else the JSON-decoded body, else the raw bytes as a string.

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

local function record_message(client, route_id, request)
    local key = session_key(client)
    if key and sessions[key] then
        sessions[key].last_message = {
            route_id = route_id,
            request = request,
            request_type = type(request),
            player_id = player_of(client),
            time = shield.now()
        }
    end
    return true
end

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

function M.get_sessions(ctx)
    return sessions
end

function M.on_client_bound(ctx, client)
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

function M.on_client_unbound(ctx, client, reason)
    local key = session_key(client)
    if key and sessions[key] then
        sessions[key].connected = false
        sessions[key].unbound_reason = reason
    end
end

-- Compiled c2s/bidi bindings (declared in the spawn opts' rpc.routes and
-- resolved at startup). The bridge delivers typed ClientIngress here; the
-- direct-call tests pass the same (client, request) shape. Route ids: 0x1001
-- (= 4097) carries structured/JSON-decodable payloads, 0x1002 raw strings.
function M.gw_move(ctx, client, request)
    return record_message(client, 0x1001, request)
end

function M.gw_raw_echo(ctx, client, request)
    return record_message(client, 0x1002, request)
end

return M
