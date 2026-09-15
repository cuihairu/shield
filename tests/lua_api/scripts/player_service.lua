-- shield_player P0 test service (LAPI-011).
--
-- One module plays both roles from the runtime-player.md contract: the
-- auth-entry service (drives player:authenticate inside a pre-login route)
-- and the player-instance service (spawned by authenticate as
-- player_<uid>). All hooks record their calls into M.calls so the C++ test
-- can assert hook dispatch per VM.
--
-- NOTE on signatures: methods dispatched through manager.call always receive
-- the dispatch ctx table as their first argument (invoke_coroutine
-- prepend_ctx), so every test entry point below takes ctx first and the
-- business parameters follow. Hooks invoked directly by the framework
-- (auth/login/...) keep their documented (ctx, ...) signatures; on_init keeps
-- (args); player_save is called directly by defaults.save.
local M = {}

local player  -- the setup() facade, per VM

-- Per-VM hook call log: {{name=..., a=..., b=...}, ...}
M.calls = {}

local function record(name, a, b)
    table.insert(M.calls, {name = name, a = a, b = b})
end

function M.on_init(args)
    M.init_args = args or {}
end

-- Instance bootstrap: authenticate spawns instances named player_<uid>
-- running this same script. The framework calls on_init once at spawn,
-- before any setup() could have wrapped it, so the first on_init performs
-- the setup itself and replays the spawn args through the fresh wrapper
-- (which stores pending_auth_result/pending_device_id for the coming Bound
-- control message). Spawn wraps the caller's args as
-- {name=..., id=..., args={...}, config=...}, so unwrap one level.
local bootstrapped = false
local raw_on_init = M.on_init
function M.on_init(args)
    local inner = type(args) == 'table' and type(args.args) == 'table'
        and args.args or args
    if not bootstrapped and type(inner) == 'table' and inner.player_id then
        bootstrapped = true
        M.do_setup('full', nil, nil)
        M.on_init(inner)
        return
    end
    raw_on_init(args)
end

-- ---------------------------------------------------------------------------
-- hooks
-- ---------------------------------------------------------------------------

function M.auth(ctx, client, request)
    record('auth', request and request.player_id)
    if request and request.deny then
        return false, request.code or 'custom_denied'
    end
    if request and request.bad_result then
        return {not_a_player_id = true}
    end
    if request and request.anonymous then
        return {player_id = request.player_id or 'anon-1', anonymous = true}
    end
    if request and request.spectator then
        return {player_id = request.player_id or 'spec-1', spectator = true}
    end
    return true, request.player_id
end

function M.login(ctx, client, auth_result)
    record('login', auth_result and auth_result.player_id)
end

function M.client_message(ctx, client, route_name, request)
    record('client_message', route_name)
    if request and request.block then
        return false, request.code or 'blocked'
    end
    return true
end

function M.disconnect(ctx, client, reason)
    record('disconnect', reason)
end

function M.logout(ctx, client, reason)
    record('logout', reason)
end

-- Optional hooks: the test overrides some via do_setup to check delegation.
function M.ready_custom(ctx, client)
    record('ready_custom')
    return shield.player.defaults.ready(ctx, client)
end

function M.reconnect_custom(ctx, client)
    record('reconnect_custom')
    return shield.player.defaults.reconnect(ctx, client)
end

-- The instance's persistence target (OD-009: player_save(uid, fields)).
function M.player_save(uid, fields)
    record('player_save', uid, fields)
end

-- ---------------------------------------------------------------------------
-- test entry points (ctx first — see the note at the top)
-- ---------------------------------------------------------------------------

-- mode: "full" (hook functions), "names" (module method names), or
-- "missing_<hook>" (drop that required hook).
function M.do_setup(ctx, mode, instance_script, instance_routes)
    mode = mode or 'full'
    local opts = {
        instance_script = instance_script,
        instance_routes = instance_routes,
        ready = M.ready_custom,
        reconnect = M.reconnect_custom,
    }
    local required = {
        auth = M.auth,
        login = M.login,
        client_message = M.client_message,
        disconnect = M.disconnect,
        logout = M.logout,
    }
    for name, fn in pairs(required) do
        if mode == 'names' then
            opts[name] = name  -- module method name form
        elseif mode == ('missing_' .. name) then
            opts[name] = nil
        else
            opts[name] = fn
        end
    end
    -- The custom ready/reconnect overrides only apply in "full" mode; in
    -- "names" mode they are exercised through defaults instead.
    if mode ~= 'full' then
        opts.ready = nil
        opts.reconnect = nil
    end
    -- setup returns (nil, error_table) on refusal — including the
    -- module-compiled-out stub, whose stable code is module_unavailable.
    local ok, setup_result, setup_err = pcall(function()
        return shield.player.setup(M, opts)
    end)
    if not ok then return {ok = false, error = tostring(setup_result)} end
    if not setup_result then
        return {ok = false,
                code = type(setup_err) == 'table' and setup_err.code
                    or 'setup_invalid'}
    end
    player = setup_result
    return {ok = true, has_auth = type(player.authenticate) == 'function',
            has_push = type(player.push) == 'function',
            has_session = type(player.session) == 'function'}
end

-- Base sugar (P2, OD-014): hooks collected from the module by their setup
-- field names. mode: "collect" (every hook present on the copy), or
-- "missing_<hook>" (that required method is dropped from the copy), or
-- "override" (an explicit opts client_message wins over the collected one).
function M.do_base_setup(ctx, mode, instance_routes)
    mode = mode or 'collect'
    local m = {}
    for _, name in ipairs({'auth', 'login', 'client_message', 'disconnect',
                           'logout'}) do
        if mode ~= ('missing_' .. name) then m[name] = M[name] end
    end
    local opts = {instance_routes = instance_routes}
    if mode == 'override' then
        opts.client_message = function(c, client, route_name, request)
            record('cm_override', route_name)
            return true
        end
    end
    local ok, setup_result, setup_err = pcall(function()
        return shield.player.Base.setup(m, opts)
    end)
    if not ok then return {ok = false, error = tostring(setup_result)} end
    if not setup_result then
        return {ok = false,
                code = type(setup_err) == 'table' and setup_err.code
                    or 'setup_invalid'}
    end
    player = setup_result
    return {ok = true, has_auth = type(player.authenticate) == 'function',
            has_push = type(player.push) == 'function'}
end

function M.do_authenticate(ctx, client, request)
    if not player then return {ok = false, code = 'not_setup'} end
    local ok, second = player:authenticate(ctx, client, request)
    if not ok then
        local code = type(second) == 'table' and second.code or tostring(second)
        return {ok = false, code = code}
    end
    -- The fresh-epoch ClientRef from bind exposes method accessors.
    return {ok = true, player = second:player_id(),
            epoch = second:session_epoch(), session = second:session_id()}
end

-- Calls the client-message guard the same way invoke_client_rpc does.
function M.do_guard(ctx, client, route, request)
    local g = rawget(_G, '__shield_player_guard')
    if not g then return {installed = false, allowed = true} end
    local ok, code = g(ctx, client, route, request)
    return {installed = true, allowed = ok == true, code = code}
end

function M.do_push(ctx, uid, route, payload)
    if not player then return false, 'not_setup' end
    return player:push(uid, route, payload)
end

function M.do_logout(ctx, reason)
    if not player then return false end
    player:logout(reason)
    return true
end

function M.set_data(ctx, key, value)
    player:set_data(key, value)
end

function M.save_now(ctx, reason)
    return shield.player.defaults.save(nil, reason)
end

function M.my_state()
    if not player then return nil end
    local s = player:session()
    return s and s.state or nil
end

function M.calls_json()
    return M.calls
end

function M.stats()
    return shield.player.stats()
end

function M.config()
    return shield.player.config()
end

function M.manager_get(ctx, uid)
    return shield.player.manager.get(uid)
end

function M.manager_size()
    return shield.player.manager.size()
end

function M.manager_register(ctx, ref, device_id, state, now_ms)
    return shield.player.manager.register_session(ref, device_id, state,
                                                  now_ms or 1000)
end

function M.manager_set_state(ctx, uid, state)
    return shield.player.manager.set_state(uid, state)
end

function M.manager_get_devices(ctx, uid)
    return shield.player.manager.get_devices(uid)
end

function M.manager_unregister(ctx, uid)
    return shield.player.manager.unregister(uid)
end

-- Sorted names of the defaults table (all must be real functions).
function M.defaults_names()
    local names = {}
    for k, v in pairs(shield.player.defaults) do
        if type(v) == 'function' then table.insert(names, k) end
    end
    table.sort(names)
    return names
end

function M.resolve(ctx, ref)
    local ok, second = shield.player.resolve(ref)
    if not ok then
        return nil, type(second) == 'table' and second.code or tostring(second)
    end
    return {uid = ok.uid, state = ok.state, service_id = ok.service_id,
            epoch = ok.epoch, device_id = ok.device_id}
end

-- PlayerRef marker -> box -> marker round trip: the argument arrives as the
-- read-only PlayerRef userdata; returning it serializes back to the marker.
function M.ref_identity(ctx, ref)
    return ref
end

function M.ref_fields(ctx, ref)
    return {uid = ref.uid, node_id = ref.node_id,
            service_id = ref.service_id, epoch = tostring(ref.epoch)}
end

function M.node_info()
    return shield.player.node_info()
end

function M.now_ms_is_number()
    return type(shield.player.now_ms()) == 'number'
end

return M
