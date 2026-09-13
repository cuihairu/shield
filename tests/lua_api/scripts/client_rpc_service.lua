-- Test service for the client RPC closed loop (LAPI client_rpc):
-- bind via shield.client.bind, egress via shield.client_rpc.<name>, and
-- gateway-initiated close via shield.client.close. Handlers run in
-- coroutines, so bind suspends on the gateway round-trip and resumes with
-- (true, ClientRef) or (false, error table).
local M = {}

function M.on_init(args) end

-- Full loop: bind, then push an s2c payload through the generated helper.
function M.login(ctx, client, player_id)
    local ok, ref_or_err = shield.client.bind(client, player_id, "player")
    if not ok then
        return {bound = false,
                code = ref_or_err and ref_or_err.code or "unknown"}
    end
    local ref = ref_or_err
    local sent =
        shield.client_rpc.login_result(ref, {welcome = ref:player_id()})
    return {bound = true, player = ref:player_id(),
            epoch = ref:session_epoch(), session = ref:session_id(),
            sent = sent}
end

function M.kick(ctx, client, reason)
    return shield.client.close(client, reason)
end

function M.egress(ctx, client, payload)
    return shield.client_rpc.login_result(client, payload)
end

-- The client.ref slot materializes the ClientRefBox face of the identity:
-- the egress helper accepts it directly and its method accessors work.
function M.ref_probe(ctx, client, player_id)
    local ok, ref = shield.client.bind(client, player_id, "player")
    if not ok then
        return {bound = false,
                code = ref and ref.code or "unknown"}
    end
    local r = ref:ref()
    if not r then return {bound = true, no_ref = true} end
    local sent = shield.client_rpc.login_result(r, {via = "refbox"})
    return {bound = true, sent = sent, proto = r:protocol_profile_id(),
            gw = r:gateway(), epoch = r:session_epoch()}
end

-- A plain __shield_client_ref marker table also egresses: the path a
-- payload takes when it crosses a boundary without userdata
-- materialization.
function M.marker_egress(ctx, client, player_id)
    local ok, ref = shield.client.bind(client, player_id, "player")
    if not ok then return false end
    -- The marker carries the post-bind epoch: an egress whose context
    -- trails the live binding is dropped as stale by the gateway.
    local marker = {__shield_client_ref = true,
                    gateway_address = ref:gateway(),
                    session_id = ref:session_id(),
                    session_epoch = ref:session_epoch(),
                    player_id = player_id,
                    protocol_profile_id = ref:protocol_profile_id()}
    local sent = shield.client_rpc.login_result(marker, {via = "marker"})
    local direct = shield._client_egress(marker, 1001, {via = "primitive"})
    return sent == true and direct == true
end

-- Raw-bytes egress through the generated helper (string payload).
function M.egress_raw(ctx, client)
    return shield.client_rpc.login_result(client, "raw-bytes")
end

-- Payload that is neither table nor string: the helper refuses.
function M.egress_bad_payload(ctx, client)
    return shield.client_rpc.login_result(client, 42)
end

-- The raw _client_egress primitive behind the helpers: same payload rules,
-- but the route id comes from the caller.
function M.primitive_egress(ctx, client, payload)
    return shield._client_egress(client, 1001, payload)
end

function M.bad_bind(ctx, client)
    local ok, err = shield.client.bind(client, "p", "player")
    return {ok = ok, code = err and err.code or nil}
end

return M
