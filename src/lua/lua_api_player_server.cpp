// [SHIELD_LUA] shield.player and shield.server API registration.
// Each module keeps its own SHIELD_ENABLE_* guard: the compiled-out
// path registers a stub table whose entries report module_unavailable.
// Split out of lua_api.cpp.
#include "lua_api_detail.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_api.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_PLAYER
#include "shield/player/player_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_GLOBAL
#include "shield/global/global_manager.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/binding.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

#ifdef SHIELD_ENABLE_PLAYER
namespace {

// sol table_proxy has no get_or_default; read fields defensively instead so
// a malformed ref table degrades to an empty field (mirroring the
// client-identity from_json behavior).
std::string player_ref_string(const shd::table& t, const char* key) {
    shd::object v = t[key];
    return v.is<std::string>() ? v.as<std::string>() : std::string();
}

std::uint64_t player_ref_epoch(const shd::table& t) {
    shd::object v = t["epoch"];
    // Epoch arrives as a decimal string (the Lua double round-trip would
    // truncate a full uint64) but accept a number for convenience.
    if (v.is<std::string>()) {
        try {
            return std::stoull(v.as<std::string>());
        } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: exception-dispatch
                   // clone arc; handler executes via malformed epoch)
            const std::exception&) {
            return 0;
        }
    }
    if (v.is<std::uint64_t>()) return v.as<std::uint64_t>();
    if (v.is<int>())  // GCOVR_EXCL_BR_LINE (unreachable defensive arm: shd
                      // is<uint64_t>() above accepts the whole Lua-integer
                      // domain, including negatives, so is<int>() never wins;
                      // tests drive string/bool/double/negative-int shapes)
        return static_cast<std::uint64_t>(
            v.as<int>());  // GCOVR_EXCL_BR_LINE + GCOVR_EXCL_LINE (unreachable)
    return 0;
}

shield::player::PlayerRef player_ref_from_table(const shd::table& t) {
    shield::player::PlayerRef ref;
    ref.uid = player_ref_string(t, "uid");
    ref.node_id = player_ref_string(t, "node_id");
    ref.service_id = player_ref_string(t, "service_id");
    ref.epoch = player_ref_epoch(t);
    return ref;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of player_ref_from_table)

// Shared snapshot shape for get/resolve: flat read-only fields plus a
// materialized PlayerRef under `ref`.
shd::table write_session(shd::state_view s,
                         const shield::player::SessionInfo& info) {
    shd::table out = s.create_table();
    out["uid"] = info.ref.uid;
    out["node_id"] = info.ref.node_id;
    out["service_id"] = info.ref.service_id;
    out["epoch"] = std::to_string(info.ref.epoch);
    out["state"] = shield::player::session_state_name(info.state);
    out["device_id"] = info.device_id;
    out["ref"] = shd::make_object(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // shd make_object dispatch clones)
        s, PlayerRefBox{{// GCOVR_EXCL_BR_LINE (compiler artifact: gcov
                         // attributes the init-list branches to this line)
                         info.ref.uid, info.ref.node_id,
                         info.ref.service_id,  // GCOVR_EXCL_BR_LINE (shd
                                               // argument-conversion arcs at
                                               // the PlayerRefBox boundary)
                         info.ref.epoch}});
    return out;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of write_session)

}  // namespace

// -----------------------------------------------------------------------------
// shield_player (P0): session admission (C++ PlayerManager) + the Lua-first
// session state machine (this orchestration chunk). See runtime-player.md,
// "shield_player 模块契约(P0)".
//
// The chunk runs once per VM. All shield.* references inside are resolved at
// CALL time: register_player_api runs before lua["shield"] is assigned.
// -----------------------------------------------------------------------------
constexpr const char* kPlayerOrchestration = R"lua(
local impl = {
    installed = false,
    M = nil,
    hooks = nil,
    instance_script = nil,
    session = nil,
    -- pending_* carry what the spawning side passed through spawn args and
    -- are consumed by on_bound on the player-instance VM.
    pending_auth_result = nil,
    pending_device_id = nil,
    data = {},
    stats = {rejected_not_ready = 0, rejected_by_guard = 0, offline_dropped = 0},
}

local REQUIRED = {'auth', 'login', 'client_message', 'disconnect', 'logout'}
local OPTIONAL = {'ready', 'reconnect', 'save'}

local function err(code, message)
    return {code = code, message = message or code, retryable = false}
end

local function now_ms()
    return shield.player.now_ms()
end

local function node_info()
    return shield.player.node_info()
end

local function mgr()
    return shield.player.manager
end

-- Fresh PlayerRef for a locally hosted uid (runtime-player.md: node_id and
-- epoch come from node_info(), service name follows one-player-one-service).
local function make_ref(uid)
    local n = node_info()
    return {uid = uid, node_id = n.node_id,
            service_id = 'player_' .. uid, epoch = n.epoch}
end

-- Per-VM default hook implementations (runtime-player.md 钩子表). Each one
-- is a real behavior, never a silent noop: business hooks may delegate back
-- to them explicitly via shield.player.defaults.*.
local defaults = {}

function defaults.allow(ctx, client, route_name, request)
    return true
end

function defaults.ready(ctx, client)
    if impl.session then
        impl.session.state_name = 'ready'
        mgr().set_state(impl.session.uid, 'ready')
    end
    return true
end

function defaults.reconnect(ctx, client)
    if not impl.session then return false end
    -- The restored session's live ClientRef is the fresh one the spawner
    -- bound; queued offline messages must egress through it, not the stale
    -- pre-disconnect ref.
    if client then impl.session.client_ref = client end
    impl.session.state_name = 'ready'
    mgr().set_state(impl.session.uid, 'ready')
    impl.flush_offline()
    return true
end

function defaults.save(ctx, reason)
    -- persistence (OD-009): serialize whitelist fields into the instance's
    -- player_save(uid, fields) when the business provides one; with no
    -- persistence configured this is an honest no-op.
    if not impl.session then return true end
    local cfg = shield.player.config()
    local allow = {}
    for _, k in ipairs(cfg.persistence_fields or {}) do allow[k] = true end
    local fields = {}
    for k, v in pairs(impl.data) do
        if allow[k] then fields[k] = v end
    end
    local M = impl.M
    if type(M.player_save) == 'function' then
        local ok, e = pcall(M.player_save, impl.session.uid, fields)
        if not ok then
            if cfg.persistence_panic then
                error('persistence_save_failed: ' .. tostring(e))
            end
            shield.log.warn('persistence_save_failed: ' .. tostring(e))
        end
    end
    return true
end

function impl.push_online(client_ref, route_name, payload)
    local helper = shield.client_rpc[route_name]
    if type(helper) ~= 'function' then return false end
    return helper(client_ref, payload) == true
end

function impl.flush_offline()
    local s = impl.session
    if not s then return end
    local q = s.offline
    s.offline = {}
    for i = 1, #q do
        local item = q[i]
        if not impl.push_online(s.client_ref, item.route, item.payload) then
            impl.stats.offline_dropped = impl.stats.offline_dropped + 1
        end
    end
end

function impl.save_now(reason)
    local ok, e = pcall(impl.hooks.save, nil, reason)
    if not ok then
        shield.log.warn('player save hook failed: ' .. tostring(e))
    end
end

function impl.do_logout(ctx, client, reason)
    local s = impl.session
    if not s then return end
    impl.session = nil
    mgr().unregister(s.uid)
    local ok, e = pcall(impl.hooks.logout, ctx, client, reason)
    if not ok then
        shield.log.warn('player logout hook failed: ' .. tostring(e))
    end
    impl.save_now('logout')
    s.offline = {}
end

-- Bound control message on a player-instance VM: first bind of the session.
function impl.on_bound(ctx, client)
    if not impl.installed then return end
    if impl.session then return end
    -- ClientContext userdata exposes identity through method accessors
    -- (runtime-player.md: client:player_id()); a marker table would carry
    -- them as fields. Handles both.
    local function client_field(c, name)
        if type(c) == 'userdata' then
            local ok, v = pcall(c[name], c)
            if ok then return v end
            return nil
        elseif type(c) == 'table' then
            return c[name]
        end
        return nil
    end
    local uid = client_field(client, 'player_id')
    if not uid or uid == '' then return end
    local s = {
        uid = uid,
        service_name = 'player_' .. uid,
        state_name = 'online',
        offline = {},
        client_ref = client_field(client, 'ref'),
        device_id = impl.pending_device_id or '',
    }
    impl.session = s
    local n = node_info()
    mgr().register_session(
        {uid = uid, node_id = n.node_id, service_id = s.service_name,
         epoch = n.epoch},
        s.device_id, 'online', now_ms())
    local auth_result = impl.pending_auth_result or {}
    impl.pending_auth_result = nil
    impl.pending_device_id = nil
    impl.hooks.login(ctx, client, auth_result)
    impl.hooks.ready(ctx, client)
end

function impl.on_disconnected(ctx, client, reason)
    if not impl.installed or not impl.session then return end
    local uid = impl.session.uid
    impl.session.state_name = 'disconnected'
    mgr().mark_disconnected(uid, now_ms())
    local ok, e = pcall(impl.hooks.disconnect, ctx, client, reason)
    if not ok then
        shield.log.warn('player disconnect hook failed: ' .. tostring(e))
    end
    local cfg = shield.player.config()
    local window = cfg.reconnect_window_ms or 30000
    if window > 0 then
        shield.timer_once(window, function()
            if impl.session and impl.session.uid == uid and
                impl.session.state_name == 'disconnected' then
                impl.do_logout(nil, nil, 'timeout')
            end
        end)
    else
        impl.do_logout(nil, nil, 'timeout')
    end
end

function impl.on_unbound(ctx, client, reason)
    if not impl.installed or not impl.session then return end
    impl.do_logout(ctx, client, reason or 'unbound')
end

-- player:authenticate(ctx, client, request) — called inside a pre-login
-- route handler coroutine on the auth-entry service (runtime-player.md
-- 认证与状态机, 5 steps).
function impl.authenticate(ctx, client, request)
    if not impl.installed then
        return false, err('module_unavailable', 'player not set up')
    end
    -- 1. auth hook: table result, or true+player_id, or false/nil+code.
    local r = table.pack(impl.hooks.auth(ctx, client, request))
    if r[1] == false or r[1] == nil then
        local code = type(r[2]) == 'string' and r[2] or 'auth_failed'
        return false, err(code, 'auth rejected: ' .. code)
    end
    local auth_result = r[1]
    if auth_result == true then auth_result = {player_id = r[2]} end
    if type(auth_result) ~= 'table' or type(auth_result.player_id) ~= 'string'
        or auth_result.player_id == '' then
        return false, err('auth_failed',
                          'auth hook must yield a non-empty player_id')
    end
    -- 2. anonymous/spectator opt-in gate.
    local cfg = shield.player.config()
    if auth_result.anonymous and not cfg.anonymous then
        return false, err('anonymous_disabled',
                          'anonymous logins are disabled')
    end
    if auth_result.spectator and not cfg.spectator then
        return false, err('spectator_disabled',
                          'spectator logins are disabled')
    end
    -- 3. PlayerManager admission ruling. The device fingerprint travels on
    -- the login request unless the auth hook attached one to its result.
    local uid = auth_result.player_id
    local device_id = auth_result.device_id
        or (type(request) == 'table' and request.device_id) or ''
    local now = now_ms()
    local d = mgr().admit(uid, device_id, now)
    if not d then
        return false, err('module_unavailable', 'player manager unavailable')
    end
    if d.kind == 'reject' then
        return false, err(d.code, 'admission rejected')
    end
    if d.kind == 'kick_old' and d.kicked_service_id and
        d.kicked_service_id ~= '' then
        shield.send(d.kicked_service_id, 'player_logout',
                    {reason = 'replaced'})
    end
    local ref = make_ref(uid)
    if d.kind == 'restore' then
        -- Reconnect-window restore: reuse the live instance, no respawn.
        local ok, second = shield.client.bind(client, uid, ref.service_id)
        if not ok then return false, second end
        mgr().mark_reconnected(uid, now)
        -- The instance must flush through the fresh-epoch ref bind just
        -- produced: the caller's client still carries the pre-reconnect
        -- epoch and the gateway drops its egress as stale.
        shield.send(ref.service_id, 'player_reconnect', {client = second})
        return true, second
    end
    -- 4. fresh spawn (one-player-one-service) + bind.
    if type(impl.instance_script) ~= 'string' or
        impl.instance_script == '' then
        return false, err('instance_script_required',
                          'setup opts.instance_script is required to spawn '
                              .. 'player instances')
    end
    local spawn_opts = {name = ref.service_id,
                        args = {player_id = uid, device_id = device_id,
                                auth_result = auth_result}}
    if impl.instance_routes then
        spawn_opts.rpc = {routes = impl.instance_routes}
    end
    local _, spawn_err = shield.spawn(impl.instance_script, spawn_opts)
    if spawn_err then return false, spawn_err end
    local ok, second = shield.client.bind(client, uid, ref.service_id)
    if not ok then return false, second end
    -- 5. success: the fresh-epoch ClientRef from bind.
    return true, second
end

function impl.push(target, route_name, payload)
    if not impl.installed then
        return false, err('module_unavailable', 'player not set up')
    end
    local uid
    if type(target) == 'string' then
        uid = target
    elseif type(target) == 'userdata' or type(target) == 'table' then
        uid = target.uid
    end
    if not uid or uid == '' then
        return false, err('invalid_player_ref',
                          'push target must be a uid or PlayerRef')
    end
    if impl.session and impl.session.uid == uid then
        if impl.session.state_name == 'ready' or
            impl.session.state_name == 'online' then
            if impl.push_online(impl.session.client_ref, route_name,
                                payload) then
                return true
            end
            return false, err('push_route_not_found',
                              'no s2c helper for route: ' ..
                                  tostring(route_name))
        end
        -- Offline (disconnected window): queue for the reconnect flush.
        local cfg = shield.player.config()
        local limit = cfg.message_queue_limit or 64
        if #impl.session.offline >= limit then
            impl.stats.offline_dropped = impl.stats.offline_dropped + 1
            return false, err('offline_queue_full',
                              'message queue limit reached')
        end
        table.insert(impl.session.offline,
                     {route = route_name, payload = payload})
        return true
    end
    return false, err('player_not_found',
                      'player is not hosted by this service')
end

function impl.setup(M, opts)
    if type(M) ~= 'table' then
        return nil, err('setup_invalid', 'module table required')
    end
    if impl.installed then
        return nil, err('setup_invalid', 'player module already set up')
    end
    opts = opts or {}
    local hooks = {}
    for _, name in ipairs(REQUIRED) do
        local v = opts[name]
        if type(v) == 'function' then
            hooks[name] = v
        elseif type(v) == 'string' and type(M[v]) == 'function' then
            hooks[name] = M[v]
        else
            return nil, err('setup_invalid',
                            'missing required hook: ' .. name)
        end
    end
    for _, name in ipairs(OPTIONAL) do
        local v = opts[name]
        if type(v) == 'function' then
            hooks[name] = v
        elseif type(v) == 'string' and type(M[v]) == 'function' then
            hooks[name] = M[v]
        else
            hooks[name] = defaults[name]
        end
    end
    impl.hooks = hooks
    impl.M = M
    impl.instance_script = opts.instance_script
    impl.instance_routes = opts.instance_routes
    impl.installed = true

    -- Session lifecycle: wrap the gateway control-message methods so the
    -- gateway-layer module hooks (business on_client_bound etc.) and the
    -- player hooks keep working side by side (runtime-player.md).
    local prev_bound = M.on_client_bound
    M.on_client_bound = function(ctx, client)
        if prev_bound then prev_bound(ctx, client) end
        impl.on_bound(ctx, client)
    end
    local prev_disc = M.on_disconnect
    M.on_disconnect = function(ctx, client, reason)
        if prev_disc then prev_disc(ctx, client, reason) end
        impl.on_disconnected(ctx, client, reason)
    end
    local prev_unbound = M.on_client_unbound
    M.on_client_unbound = function(ctx, client, reason)
        if prev_unbound then prev_unbound(ctx, client, reason) end
        impl.on_unbound(ctx, client, reason)
    end
    -- spawn args reach the instance through on_init (bootstrap-like).
    local prev_init = M.on_init
    M.on_init = function(args)
        if prev_init then prev_init(args) end
        args = args or {}
        impl.pending_auth_result = args.auth_result
        impl.pending_device_id = args.device_id
    end
    -- Framework-driven instance methods (business may not override these
    -- names; they carry the replaced/timeout logout and reconnect paths).
    -- Both arrive through shield.send, whose dispatch always prepends the
    -- ctx table before the caller's arg table.
    M.player_logout = function(ctx, args)
        local reason = type(args) == 'table' and args.reason or args
        impl.do_logout(ctx, nil, reason or 'logout')
    end
    M.player_reconnect = function(ctx, args)
        if impl.installed and impl.session then
            local client = type(args) == 'table' and args.client or nil
            impl.hooks.reconnect(ctx, client)
        end
    end

    -- Periodic save (0 = logout-only).
    local cfg = shield.player.config()
    if (cfg.save_interval_ms or 0) > 0 then
        shield.timer(cfg.save_interval_ms, function()
            if impl.session then impl.save_now('interval') end
        end)
    end

    -- The client-message guard for invoke_client_rpc (see lua_runtime.cpp).
    -- Sessions that do not exist on this VM (the auth-entry service, or any
    -- message before the first bind) pass through untouched. Ready-state
    -- truth lives in the PlayerManager so every observer agrees on it.
    rawset(_G, '__shield_player_guard', function(ctx, client, route_name,
                                                 request)
        if not impl.session then return true end
        local state_name = impl.session.state_name
        local sess = mgr().get(impl.session.uid)
        if sess then state_name = sess.state end
        if state_name ~= 'ready' then
            impl.stats.rejected_not_ready = impl.stats.rejected_not_ready + 1
            return false, 'not_ready'
        end
        local ok, code = impl.hooks.client_message(ctx, client, route_name,
                                                   request)
        if ok then return true end
        impl.stats.rejected_by_guard = impl.stats.rejected_by_guard + 1
        return false, code or 'rejected'
    end)

    -- The per-module facade.
    local f = {}
    function f:authenticate(ctx, client, request)
        return impl.authenticate(ctx, client, request)
    end
    function f:push(target, route_name, payload)
        return impl.push(target, route_name, payload)
    end
    function f:set_data(key, value) impl.data[key] = value end
    function f:get_data(key) return impl.data[key] end
    function f:session()
        if not impl.session then return nil end
        local s = impl.session
        return {uid = s.uid, state = s.state_name, device_id = s.device_id,
                service_id = s.service_name}
    end
    function f:logout(reason)
        return impl.do_logout(nil, nil, reason or 'logout')
    end
    return f
end

-- shield.player.Base (P2 syntax sugar, OD-014): collects the hooks from the
-- module table by their setup field names and delegates to impl.setup — no
-- second lifecycle, no inheritance, no on_* prefix. opts carries only the
-- non-hook options (instance_script/instance_routes); an explicit hook
-- function in opts wins over the same-named module method.
-- Base-library only on purpose: this loop runs at REGISTRATION time (the
-- chunk executes once per VM inside register_player_api), and a host VM
-- may have opened nothing beyond the base library — table.insert would
-- fail there and abort the registration. Length-append needs no library.
local ALL_HOOKS = {}
for _, name in ipairs(REQUIRED) do ALL_HOOKS[#ALL_HOOKS + 1] = name end
for _, name in ipairs(OPTIONAL) do ALL_HOOKS[#ALL_HOOKS + 1] = name end

local Base = {}
function Base.setup(M, opts)
    local merged = {}
    for _, name in ipairs(ALL_HOOKS) do
        if type(M[name]) == 'function' then merged[name] = M[name] end
    end
    if type(opts) == 'table' then
        for _, key in ipairs({'instance_script', 'instance_routes'}) do
            merged[key] = opts[key]
        end
        for _, name in ipairs(ALL_HOOKS) do
            if type(opts[name]) == 'function' then merged[name] = opts[name] end
        end
    end
    return impl.setup(M, merged)
end

return {setup = impl.setup, defaults = defaults, impl = impl, Base = Base}
)lua";

void register_player_api(shd::table& shield, LuaServiceManager* manager) {
    shd::state_view lua(shield.lua_state());

    // Run the orchestration chunk once per VM; it returns the impl table.
    // The conversion is validity-guarded: the previous binding layer's
    // implicit protected_function_result -> object conversion type-panics
    // (and, in plain C++ registration code outside any lua_pcall, aborts the
    // process) when the chunk failed. A failed run degrades to an empty
    // impl table; shield.player.setup then reports its setup_invalid
    // error through the normal protected-call path.
    shd::protected_function_result run = shd::safe_script(
        lua,
        kPlayerOrchestration,  // GCOVR_EXCL_LINE (gcov clone artifact: the
                               // argument-load line is emitted only into an
                               // outlined clone that is never called; the
                               // call itself is counted on the closing line)
        [](lua_State*,         // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    shd::table impl = run.valid() ? run.get<shd::table>() : lua.create_table();

    auto player = lua.create_table();

    player.set_function(
        "setup",
        [impl](shd::this_state state, shd::table M,
               shd::object opts) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            shd::protected_function setup = impl["setup"];
            shd::protected_function_result r =
                opts.valid() ? setup(M, opts) : setup(M);
            if (!r.valid()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "setup_invalid",
                                             "player setup raised an error"));
                return results;
            }
            for (unsigned int i = 0; i < r.return_count(); ++i) {
                results.push_back(r.get<shd::object>(i));
            }
            return results;
        });

    player.set_function("config", [](shd::this_state state) -> shd::table {
        shd::state_view s(state);
        auto* pm = shield::player::PlayerManager::global();
        shd::table cfg = s.create_table();
        if (pm == nullptr) return cfg;
        const auto& c = pm->config();
        cfg["multi_device"] =  // GCOVR_EXCL_LINE (gcov attributes no code to
                               // this line)
            c.multi_device == shield::player::MultiDevicePolicy::kSingle
                ? "single"
                : (c.multi_device == shield::player::MultiDevicePolicy::kKickOld
                       ? "kick_old"
                       : "multi");
        cfg["max_devices"] = c.max_devices;
        cfg["anonymous"] = c.anonymous_enabled;
        cfg["spectator"] = c.spectator_enabled;
        cfg["reconnect_window_ms"] = c.reconnect_window_ms;
        cfg["message_queue_limit"] = c.message_queue_limit;
        shd::table persistence = s.create_table();
        persistence["binding"] = c.persistence_binding;
        persistence["panic"] = c.persistence_panic_on_error;
        persistence["save_interval_ms"] = c.save_interval_ms;
        shd::table fields = s.create_table();
        int i = 1;
        for (const auto& f : c.persistence_fields) {
            fields[i++] = f;
        }
        persistence["fields"] = fields;
        cfg["persistence"] = persistence;
        return cfg;
    });

    // Wall-clock milliseconds for the manager's window arithmetic.
    player.set_function("now_ms", []() -> std::uint64_t {
        const auto now = std::chrono::system_clock::now();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count());
    });

    // Locality for PlayerRef values. epoch travels as a decimal string:
    // uint64 does not survive the Lua double round-trip (see the cluster
    // node_epoch() binding for the same decision).
    player.set_function("node_info", [](shd::this_state state) -> shd::table {
        shd::state_view s(state);
        auto* pm = shield::player::PlayerManager::global();
        shd::table info = s.create_table();
        info["node_id"] = pm ? pm->node_id() : std::string();
        info["epoch"] = std::to_string(pm ? pm->node_epoch() : 0);
        return info;
    });  // GCOVR_EXCL_LINE (function-exit arc artifact of the node_info
         // lambda)

    player.set_function("stats", [impl](shd::this_state state) -> shd::table {
        shd::state_view s(state);
        // The chunk returns the wrapper {setup, defaults, impl}; the
        // orchestration state (and its stats counters) lives one level
        // deeper, next to what the Lua-side closures mutate.
        shd::object wrapper = impl.raw_get<shd::object>("impl");
        shd::object impl_stats =
            wrapper.is<shd::table>()
                ? wrapper
                      .as<shd::table>()  // GCOVR_EXCL_BR_LINE (defensive:
                                         // the orchestration chunk always
                                         // returns the impl table)
                      .raw_get<shd::object>("stats")
                : shd::nil;  // GCOVR_EXCL_BR_LINE (defensive: the orchestration
                             // chunk always returns the impl table)
        shd::table out = s.create_table();
        if (impl_stats.is<shd::table>()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                            // impl.stats is unconditional in
                                            // the chunk)
            shd::table stats_tbl = impl_stats;
            // Read each counter explicitly: a proxy-to-proxy assignment
            // (out["k"] = stats_tbl["k"]) pushes nothing and silently
            // produces an empty table.
            out.raw_set("rejected_not_ready",
                        stats_tbl.get<shd::object>("rejected_not_ready"));
            out.raw_set("rejected_by_guard",
                        stats_tbl.get<shd::object>("rejected_by_guard"));
            out.raw_set("offline_dropped",
                        stats_tbl.get<shd::object>("offline_dropped"));
        }
        return out;
    });

    // ---- manager: the uid index operations ----
    shd::table manager_tbl = lua.create_table();

    manager_tbl.set_function(
        "admit",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid,        // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string device_id,  // GCOVR_EXCL_LINE (gcov attributes no code
                                   // to this line)
           std::uint64_t now_ms) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "player manager unavailable"));
                return results;
            }
            auto d = pm->admit(uid, device_id, now_ms);
            shd::table out = s.create_table();
            switch (d.kind) {  // GCOVR_EXCL_BR_LINE (no-match arc; every
                               // AdmissionDecision kind is listed)
                case shield::player::AdmissionDecision::Kind::kAllow:
                    out["kind"] = "allow";
                    break;
                case shield::player::AdmissionDecision::Kind::kRestore:
                    out["kind"] = "restore";
                    break;
                case shield::player::AdmissionDecision::Kind::kKickOld:
                    out["kind"] = "kick_old";
                    break;
                case shield::player::AdmissionDecision::Kind::kReject:
                    out["kind"] = "reject";
                    break;
            }
            out["code"] = d.code;
            out["kicked_service_id"] = d.kicked_service_id;
            results.push_back(shd::make_object(s, out));
            results.push_back(shd::make_object(s, shd::nil));
            return results;
        });

    manager_tbl.set_function(
        "register_session",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           shd::table ref,         // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string device_id,  // GCOVR_EXCL_LINE (gcov attributes no code
                                   // to this line)
           std::string state_name, std::uint64_t now_ms) -> shd::object {
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return shd::make_object(state, shd::nil);
            const shield::player::PlayerRef player_ref =
                player_ref_from_table(ref);
            auto state_of = [](const std::string& name) {
                if (name == "online")
                    return shield::player::SessionState::kOnline;
                if (name == "ready")
                    return shield::player::SessionState::kReady;
                if (name == "anonymous")
                    return shield::player::SessionState::kAnonymous;
                if (name == "spectator")
                    return shield::player::SessionState::kSpectator;
                return shield::player::SessionState::kConnecting;
            };
            pm->register_session(player_ref, device_id, state_of(state_name),
                                 now_ms);
            return shd::make_object(state, true);
        });

    manager_tbl.set_function(
        "mark_disconnected",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::uint64_t now_ms) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            return pm != nullptr && pm->mark_disconnected(uid, now_ms);
        });

    manager_tbl.set_function(
        "mark_reconnected",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::uint64_t now_ms) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            return pm != nullptr && pm->mark_reconnected(uid, now_ms);
        });

    manager_tbl.set_function(
        "unregister",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid)        // GCOVR_EXCL_LINE (lambda entry artifact)
        -> shd::object {  // GCOVR_EXCL_LINE (gcov attributes no code to
                          // this line)
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return shd::make_object(state, shd::nil);
            auto sid = pm->unregister(uid);
            if (!sid.has_value()) return shd::make_object(state, shd::nil);
            return shd::make_object(state, *sid);
        });

    manager_tbl.set_function(      // GCOVR_EXCL_LINE (gcov continuation
        "get",                     // artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid)        // GCOVR_EXCL_LINE (lambda entry artifact)
        -> shd::object {  // GCOVR_EXCL_LINE (gcov attributes no code to
                          // this line)
            auto* pm = shield::player::PlayerManager::global();
            shd::state_view s(state);
            if (pm == nullptr) return shd::make_object(state, shd::nil);
            auto info = pm->get(uid);
            if (!info.has_value()) return shd::make_object(state, shd::nil);
            return shd::make_object(s, write_session(s, *info));
        });

    manager_tbl.set_function(
        "get_devices",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid) -> shd::table {  // GCOVR_EXCL_LINE (gcov attributes
                                             // no code to this line)
            shd::state_view s(state);
            shd::table out = s.create_table();
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return out;
            int i = 1;
            for (const auto& d : pm->get_devices(uid)) {
                out[i++] = d;
            }
            return out;
        });  // GCOVR_EXCL_LINE (function-exit arc artifact of the devices
             // lambda)

    manager_tbl.set_function(
        "set_state",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::string state_name) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return false;
            shield::player::SessionState parsed =
                shield::player::SessionState::kConnecting;
            if (std::string(state_name) == "online")
                parsed = shield::player::SessionState::kOnline;
            else if (std::string(state_name) == "ready")
                parsed = shield::player::SessionState::kReady;
            else if (std::string(state_name) == "anonymous")
                parsed = shield::player::SessionState::kAnonymous;
            else if (std::string(state_name) == "spectator")
                parsed = shield::player::SessionState::kSpectator;
            else if (std::string(state_name) == "disconnected")
                parsed = shield::player::SessionState::kDisconnected;
            else if (std::string(state_name) == "authenticating")
                parsed = shield::player::SessionState::kAuthenticating;
            else if (std::string(state_name) != "connecting")
                return false;
            return pm->set_state(uid, parsed);
        });

    manager_tbl.set_function("size", [](shd::this_state state) -> std::size_t {
        auto* pm = shield::player::PlayerManager::global();
        return pm ? pm->size() : 0;
    });

    player["manager"] = manager_tbl;

    // ---- resolve / get: local-only (P0) ----
    player.set_function(
        "resolve",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,   // GCOVR_EXCL_LINE (lambda entry artifact)
           shd::object ref)         // GCOVR_EXCL_LINE (lambda entry artifact)
        -> shd::variadic_results {  // GCOVR_EXCL_LINE (gcov attributes no
                                    // code to this line)
            shd::state_view s(state);
            shd::variadic_results results;
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "player manager unavailable"));
                return results;
            }
            shield::player::PlayerRef player_ref;
            if (ref.is<PlayerRefBox>()) {
                const PlayerRefData& d = ref.as<const PlayerRefBox&>().data;
                player_ref.uid = d.uid;
                player_ref.node_id = d.node_id;
                player_ref.service_id = d.service_id;
                player_ref.epoch = d.epoch;
            } else if (ref.is<shd::table>()) {
                player_ref = player_ref_from_table(ref);
            } else {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "invalid_player_ref",
                                             "ref must be a PlayerRef"));
                return results;
            }
            if (player_ref.uid.empty()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "invalid_player_ref",
                                             "ref.uid is required"));
                return results;
            }
            if (!player_ref.node_id.empty() &&
                player_ref.node_id !=  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inline std::string compare clone;
                                       // local/remote arms driven)
                    pm->node_id()) {   // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // gcov attributes the compare-dispatch
                                       // clones to the operand line)
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(
                    state, "remote_resolve_unimplemented",
                    "remote resolve is not part of the P0 contract"));
                return results;
            }
            auto info = pm->resolve(player_ref);
            if (!info.has_value()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "player_not_found",
                               "no local session for uid: " + player_ref.uid));
                return results;
            }
            results.push_back(shd::make_object(s, write_session(s, *info)));
            results.push_back(shd::make_object(s, shd::nil));
            return results;
        });

    player.set_function(
        "get",
        [manager_tbl](shd::this_state state, std::string uid) -> shd::object {
            shd::protected_function get = manager_tbl["get"];
            return get(uid);
        });

    player["defaults"] = impl["defaults"];
    player["Base"] = impl["Base"];

    shield["player"] = player;
}
#else
// Module compiled out: every shield.player.* entry reports
// module_unavailable (LAPI-011 前言) instead of being a nil field.
void register_player_stub_api(shd::table& shield, shd::state_view lua) {
    auto unavailable = shd::safe_script(
        lua,
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_player is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    auto player = lua.create_table();
    for (const char* name : {"setup", "resolve", "get", "config", "now_ms",
                             "node_info", "stats"}) {
        player[name] = unavailable;
    }
    auto manager = lua.create_table();
    for (const char* name :
         {"admit", "register_session", "mark_disconnected", "mark_reconnected",
          "unregister", "get", "get_devices", "set_state", "size"}) {
        manager[name] = unavailable;
    }
    player["manager"] = manager;
    auto defaults = lua.create_table();
    for (const char* name : {"allow", "ready", "reconnect", "save"}) {
        defaults[name] = unavailable;
    }
    player["defaults"] = defaults;
    // Base stays a table (its sugar shape is meaningless without the
    // module), but calling Base.setup reports the same stable code.
    auto base = lua.create_table();
    base["setup"] = unavailable;
    player["Base"] = base;
    shield["player"] = player;
}
#endif

#ifdef SHIELD_ENABLE_SERVER
// -----------------------------------------------------------------------------
// shield_server (P0): C++ ServerManager state truth + this per-VM
// orchestration chunk holding the watcher callbacks. See runtime-server.md.
//
// The chunk runs once per VM. register_server_api runs before
// lua["shield"] is assigned; all shield.* references resolve at CALL time.
// The C++ watch registry only stores {watch_id, service_id} (OD-016: no
// shd::function in shield_server), so this chunk owns the callbacks and
// installs the M.on_server_state_change forwarder the C++ notify path
// (send_system dispatch) calls into.
//
// The chunk also publishes its api table as a per-VM global
// (__shield_server_impl). The C++ facade resolves it at CALL time from the
// invoking state and must never capture it in a C++ lambda: a captured
// shd::table outlives the register call (the lambda userdata lives until
// GC finalizes it), and dereferencing that reference from the finalizer
// races/aliases the VM teardown (the same dangling-reference class the
// sol-reference-coroutine anchor rule exists for — long-lived C++ holders
// never keep sol references; resolve at call time instead).
// -----------------------------------------------------------------------------
constexpr const char* kServerOrchestration = R"lua(
local impl = {watchers = {}, prev = nil, installed = false}

local api = {
    -- Store the callback and (once per VM) install the forwarder the
    -- system-message dispatch calls into. Re-watching an existing id is
    -- idempotent: the original callback is kept.
    attach = function(M, watch_id, fn)
        if impl.watchers[watch_id] then return true end
        impl.watchers[watch_id] = fn
        if not impl.installed then
            impl.installed = true
            local prev = rawget(M, 'on_server_state_change')
            if type(prev) == 'function' then impl.prev = prev end
            M.on_server_state_change = function(ctx, new_state)
                if impl.prev then
                    local ok, err = pcall(impl.prev, ctx, new_state)
                    if not ok then
                        shield.log.warn('on_server_state_change failed: ' ..
                                            tostring(err))
                    end
                end
                for _, watcher in pairs(impl.watchers) do
                    local wok, werr = pcall(watcher, ctx, new_state)
                    if not wok then
                        shield.log.warn('server watch callback failed: ' ..
                                            tostring(werr))
                    end
                end
            end
        end
        return true
    end,
    detach = function(watch_id)
        impl.watchers[watch_id] = nil
        return true
    end,
}

rawset(_G, '__shield_server_impl', api)
return api
)lua";

void register_server_api(shd::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    shd::state_view lua(shield.lua_state());

    // Run the orchestration chunk once per VM; it publishes the impl table
    // as the per-VM __shield_server_impl global, which the watch/unwatch
    // facades resolve at CALL time (never captured: see the chunk comment).
    shd::safe_script(
        lua, kServerOrchestration,
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE

    auto server = lua.create_table();

    // ---- read-only runtime info ----
    server.set_function("state", [](shd::this_state state) -> shd::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(state, shd::nil);
        }
        return shd::make_object(state,
                                shield::server::server_state_name(sm->state()));
    });

    server.set_function("uptime", [](shd::this_state state) -> shd::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(state, shd::nil);
        }
        return shd::make_object(state, sm->uptime_seconds());
    });

    server.set_function("version", [](shd::this_state state) -> shd::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(state, shd::nil);
        }
        return shd::make_object(state, sm->version());
    });

    server.set_function("node_id", [](shd::this_state state) -> shd::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(state, shd::nil);
        }
        return shd::make_object(state, sm->node_id());
    });

    server.set_function("started_at", [](shd::this_state state) -> shd::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(state, shd::nil);
        }
        return shd::make_object(state, sm->started_at_ms());
    });

    server.set_function("config", [](shd::this_state state) -> shd::object {
        shd::state_view s(state);
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return shd::make_object(s, shd::nil);
        }
        const auto& c = sm->config();
        shd::table cfg = s.create_table();
        cfg["name"] = c.name;
        shd::table info = s.create_table();
        info["name"] = c.info_name;
        info["version"] = c.info_version;
        info["region"] = c.info_region;
        cfg["info"] = info;
        return shd::make_object(s, cfg);
    });

    // ---- state control ----
    server.set_function(
        "set_state",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           std::string name) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            shield::server::ServerState parsed;
            if (!shield::server::parse_server_state(name, &parsed)) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "invalid_state",
                                             "unknown server state: " + name));
                return results;
            }
            std::string error;
            if (!sm->set_state(parsed, &error)) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "invalid_state_transition", error));
                return results;
            }
            results.push_back(shd::make_object(s, true));
            return results;
        });

    server.set_function(
        "shutdown",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::object delay) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            // The delay must be a non-negative integer number of
            // milliseconds. Lua numbers are doubles, so check through the
            // double view: negative and fractional values are rejected
            // here (an unsigned integer view would wrap negative values).
            std::uint64_t delay_ms = 0;
            bool valid = false;
            if (delay.is<double>()) {
                const double raw = delay.as<double>();
                if (raw >= 0.0 && raw == std::floor(raw) &&
                    raw <= 9007199254740992.0) {
                    delay_ms = static_cast<std::uint64_t>(raw);
                    valid = true;
                }
            }
            if (!valid) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(
                    state, "invalid_argument",
                    "shutdown delay must be a non-negative integer ms"));
                return results;
            }
            std::string error;
            if (!sm->schedule_shutdown(delay_ms, &error)) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "shutdown_already_scheduled", error));
                return results;
            }
            results.push_back(shd::make_object(s, true));
            return results;
        });

    // ---- state watchers ----
    server.set_function(
        "watch",
        [manager, runtime](shd::this_state state,
                           shd::object cb) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            if (!cb.is<shd::function>()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch expects a callback function"));
                return results;
            }
            // Watchers register for the calling service; that requires a
            // dispatch context (on_init or a handler).
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch requires a service context (call it from "
                               "on_init or a handler)"));
                return results;
            }
            shd::table module_tbl = shd::nil;
            if (runtime) {  // GCOVR_EXCL_BR_LINE (defensive:
                            // register_server_api is only invoked with a live
                            // runtime)
                // The calling service may still be spawning (an on_init
                // watcher): it is not in the registry yet, so prefer the
                // dispatch frame's VM before the registry lookup.
                auto vm = manager->current_service_vm();
                // GCOVR_EXCL_START (defensive: current_service_vm already
                // retries through the registry, so a null result here means
                // the service is gone and service_vm would return null too)
                if (!vm) {
                    vm = manager->service_vm(service_id);
                }
                // GCOVR_EXCL_STOP
                module_tbl = runtime->service_table(vm);
            }
            // GCOVR_EXCL_START (defensive: every dispatch context resolves
            // a vm whose module loaded — load failures never reach the
            // dispatch scope, and a null vm above leaves the nil sentinel)
            if (!module_tbl.valid()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch requires a loaded service module"));
                return results;
            }
            // GCOVR_EXCL_STOP
            const auto watch_id = sm->watch(service_id);
            // The chunk stores the callback and installs the forwarder. The
            // impl table is re-resolved from this state's globals on every
            // call (coroutines share them) instead of being captured by the
            // lambda: a captured sol reference would be dereferenced from
            // the GC finalizer that later destroys the lambda.
            shd::table impl = s.globals()["__shield_server_impl"];
            shd::protected_function attach = impl["attach"];
            attach(module_tbl, watch_id, cb);
            results.push_back(shd::make_object(s, watch_id));
            return results;
        });

    server.set_function(
        "unwatch",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::object watch_id) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            std::uint64_t id = 0;
            if (watch_id.is<std::uint64_t>()) {
                id = watch_id.as<std::uint64_t>();
            } else if (watch_id.is<double>()) {
                id = static_cast<std::uint64_t>(watch_id.as<double>());
            }
            sm->unwatch(id);
            shd::table impl = s.globals()["__shield_server_impl"];
            shd::protected_function detach = impl["detach"];
            detach(id);
            results.push_back(shd::make_object(s, true));
            return results;
        });

    shield["server"] = server;
}
#else
// Module compiled out: every shield.server.* entry reports
// module_unavailable instead of being a nil field.
void register_server_stub_api(shd::table& shield, shd::state_view lua) {
    auto unavailable = shd::safe_script(
        lua,
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_server is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    auto server = lua.create_table();
    for (const char* name :
         {"state", "uptime", "version", "node_id", "started_at", "config",
          "set_state", "shutdown", "watch", "unwatch"}) {
        server[name] = unavailable;
    }
    shield["server"] = server;
}
#endif

}  // namespace shield::lua
