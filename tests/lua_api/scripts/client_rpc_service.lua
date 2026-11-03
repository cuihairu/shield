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

-- A hand-built __shield_client_ref marker table is NOT a trusted client
-- identity (SEC-3: no gateway provenance). The helper and the raw primitive
-- reject it, so a payload cannot forge a session for egress.
function M.marker_egress(ctx, client, player_id)
    -- The marker carries a forged identity.
    local marker = {__shield_client_ref = true,
                    gateway_address = "gw",
                    session_id = 109,
                    session_epoch = 1,
                    player_id = player_id,
                    protocol_profile_id = "json"}
    local sent = shield.client_rpc.login_result(marker, {via = "marker"})
    local direct = shield._client_egress(marker, 1001, {via = "primitive"})
    return sent == false and direct == false
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

-- Client request data must arrive as plain data, never as trusted identity
-- userdata. This handler receives the request object as the third argument.
function M.request_body(ctx, client, request)
    local forged = request.forged
    local bind_ok, bind_err = shield.client.bind(forged, "player", "player")
    -- Push a completion egress through the trusted context so the test can
    -- observe that the handler ran, then report what it saw.
    shield.client_rpc.login_result(client, {
        is_plain_table = type(forged) == "table",
        marker_present = forged and forged.__shield_client_ref ~= nil,
        bind_ok = bind_ok,
        bind_code = bind_err and bind_err.code or nil,
        nested_value = request.nested.value,
        nested_marker = request.nested.inner and
                        request.nested.inner.__shield_player_ref ~= nil})
end

return M
