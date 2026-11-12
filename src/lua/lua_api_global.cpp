// [SHIELD_LUA] shield.global API registration: global data + local
// cache, locks, leaderboards, queues and the scheduler over the C++
// GlobalManager store, with the compiled-out stub under its own guard.
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

#ifdef SHIELD_ENABLE_GLOBAL
// -----------------------------------------------------------------------------
// shield_global (P0): global data + local cache, locks, leaderboards,
// queues and the scheduler over the C++ GlobalManager store.
//
// Waiting entries (lock acquire, queue pop with timeout) MUST yield the
// running coroutine instead of blocking the actor thread — a yield can
// never cross a C++ call frame, so the wait loops live in this Lua chunk
// (never inside a C++ lambda) and only the non-blocking primitives are
// C++ functions injected up front as __shield_global_primitives.
// -----------------------------------------------------------------------------
constexpr const char* kGlobalOrchestration = R"lua(
local impl = {sched = {}, sched_prev = nil, sched_installed = false}
local prim = rawget(_G, '__shield_global_primitives')

-- Wait until try_fn() returns true. timeout_ms nil (or < 0) = single
-- attempt; 0 = poll until success; n > 0 = bounded wait. Uses
-- shield.sleep (coroutine-aware) so actor threads never block.
local function wait_for(try_fn, timeout_ms, retry_ms)
  if try_fn() then return true end
  if timeout_ms == nil or timeout_ms < 0 then return false end
  local deadline = shield.now() + timeout_ms
  while true do
    local remaining = deadline - shield.now()
    if remaining <= 0 then return false end
    shield.sleep(math.min(retry_ms, remaining))
    if try_fn() then return true end
  end
end

local function timeout_error(what, timeout_ms)
  return {code = 'timeout',
          message = what .. ' timed out after ' .. tostring(timeout_ms) .. 'ms',
          retryable = true}
end

-- ---- exclusive locks (mutex / spinlock / distributed_mutex) ----
local function make_exclusive(name, opts, registry)
  opts = opts or {}
  local lock = {
    name = name,
    -- Field stays private (`_ttl`): the `ttl` name is the public reader
    -- method below, and a same-named field would be overwritten by the
    -- method definition (self.ttl would pass a function to lock_try).
    _ttl = opts.ttl or 0,
    retry = opts.retry or 20,
    _owner = registry .. ':' .. name .. ':' .. tostring({}),
  }
  function lock:try_acquire()
    return prim.lock_try(registry, name, self._owner, self._ttl)
  end
  function lock:release()
    return prim.lock_release(registry, name, self._owner)
  end
  function lock:extend(ttl)
    return prim.lock_extend(registry, name, self._owner, ttl or self._ttl)
  end
  function lock:owner()
    return prim.lock_info(registry, name)
  end
  function lock:ttl()
    local info = prim.lock_info(registry, name)
    if not info or not info.exists then return -1 end
    return info.ttl_remaining or -1
  end
  function lock:acquire(timeout)
    local waited = timeout or 0
    local ok = wait_for(function() return self:try_acquire() end, waited,
                        self.retry)
    if ok then return true end
    return false, timeout_error('lock acquire', waited)
  end
  function lock:with(fn)
    local ok, err = self:acquire()
    if not ok then return false, err end
    local results = table.pack(pcall(fn))
    self:release()
    if results[1] then
      return true, table.unpack(results, 2, results.n)
    end
    return false, results[2]
  end
  return lock
end

-- ---- reader/writer guards ----
local function make_guard(name, opts, kind, owner)
  local guard = {retry = (opts and opts.retry) or 20, _owner = owner}
  local try, release
  if kind == 'write' then
    try = function(ttl)
      return prim.rw_write_try(name, owner, ttl or (opts and opts.ttl) or 0)
    end
    release = function() return prim.rw_write_release(name, owner) end
  else
    try = function() return prim.rw_read_try(name, owner) end
    release = function() return prim.rw_read_release(name, owner) end
  end
  function guard:try_acquire() return try() end
  function guard:release() return release() end
  function guard:acquire(timeout)
    local waited = timeout or 0
    local ok = wait_for(function() return try() end, waited, self.retry)
    if ok then return true end
    return false, timeout_error('lock acquire', waited)
  end
  function guard:with(fn)
    local ok, err = self:acquire()
    if not ok then return false, err end
    local results = table.pack(pcall(fn))
    release()
    if results[1] then
      return true, table.unpack(results, 2, results.n)
    end
    return false, results[2]
  end
  return guard
end

local function make_rwlock(name, opts)
  return {
    name = name,
    read_lock = function(self)
      return make_guard(name, opts, 'read', 'r:' .. name .. ':' .. tostring({}))
    end,
    write_lock = function(self)
      return make_guard(name, opts, 'write', 'w:' .. name .. ':' .. tostring({}))
    end,
  }
end

-- ---- queue pop with timeout (single attempt / bounded wait) ----
local function pop_with_wait(pop_now, timeout)
  local msg = false
  local got = wait_for(function()
    local m = pop_now()
    if m ~= nil then msg = m return true end
    return false
  end, timeout, 10)
  if not got then return nil end
  return prim.decode(msg)
end

-- ---- queues ----
local function make_queue(name, opts)
  local q = {name = name}
  function q:push(v)
    prim.queue_push(name, prim.encode(v))
    return true
  end
  function q:push_batch(arr)
    for _, v in ipairs(arr) do prim.queue_push(name, prim.encode(v)) end
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.queue_pop_now(name) end,
                         timeout)
  end
  function q:length() return prim.queue_length(name) end
  function q:purge()
    prim.queue_purge(name)
    return true
  end
  return q
end

local function make_delay_queue(name, opts)
  local q = {name = name}
  function q:push(v, delay)
    prim.delay_push(name, prim.encode(v), delay)
    return true
  end
  function q:push_at(v, at)
    -- Contract timestamps are seconds (os.time()), the store keeps ms.
    prim.delay_push_at(name, prim.encode(v), at * 1000)
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.delay_pop_now(name) end,
                         timeout)
  end
  function q:pending() return prim.delay_pending(name) end
  function q:ready() return prim.delay_ready(name) end
  function q:purge()
    prim.delay_purge(name)
    return true
  end
  return q
end

local function make_reliable_queue(name, opts)
  opts = opts or {}
  prim.rel_configure(name, opts.max_retries or 3)
  local q = {name = name}
  function q:push(v)
    prim.rel_push(name, prim.encode(v))
    return true
  end
  function q:pop(timeout)
    local id, payload
    local got = wait_for(function()
      local i, p = prim.rel_pop_now(name)
      if i ~= nil then id = i payload = p return true end
      return false
    end, timeout, 10)
    if not got then return nil end
    local handle = {
      id = id,
      ack = function(self2) return prim.rel_ack(name, id) end,
      nack = function(self2, retry)
        return prim.rel_nack(name, id, retry or 0)
      end,
    }
    return prim.decode(payload), handle
  end
  function q:dead_letter()
    return {
      range = function(self2, from, to)
        local rows = prim.rel_dead_range(name, from, to or from)
        local out = {}
        for i, row in ipairs(rows) do out[i] = prim.decode(row.payload) end
        return out
      end,
      size = function(self2) return prim.rel_dead_size(name) end,
      purge = function(self2)
        prim.rel_dead_purge(name)
        return true
      end,
    }
  end
  return q
end

local function make_priority_queue(name, opts)
  local q = {name = name}
  -- Contract priorities: a smaller value pops first (urgent=1, normal=5,
  -- low=10); the default is the documented normal level.
  function q:push(v, priority)
    prim.priority_push(name, prim.encode(v), priority or 5)
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.priority_pop_now(name) end,
                         timeout)
  end
  function q:length() return prim.priority_length(name) end
  function q:purge()
    prim.priority_purge(name)
    return true
  end
  return q
end

-- Broadcast: the store keeps bounded history + per-group cursors; the
-- callbacks themselves live in this VM (C++ never holds Lua refs). A
-- group that misses pushes catches up out of the history on its next
-- subscribe; cross-process live fan-out is the Phase 2+ Pub/Sub shape.
local function make_broadcast_queue(name, opts)
  opts = opts or {}
  local max_history = tonumber(opts.max_history)
  if max_history and max_history > 0 then
    prim.broadcast_configure(name, math.floor(max_history))
  end
  local q = {name = name, _subs = {}}
  local function deliver(group, msg, seq)
    local sub = q._subs[group]
    if not sub then return end  -- the group unsubscribed mid-dispatch
    local ok, err = pcall(sub.fn, msg)
    if not ok then
      shield.log.warn('broadcast callback failed: ' .. tostring(err))
    end
    prim.broadcast_commit(name, group, seq)
  end
  function q:push(v)
    local payload = prim.encode(v)
    local seq = prim.broadcast_push(name, payload)
    local msg = prim.decode(payload)
    for group in pairs(q._subs) do deliver(group, msg, seq) end
    return true
  end
  function q:subscribe(group, fn)
    if type(group) ~= 'string' or type(fn) ~= 'function' then
      return nil, {code = 'invalid_argument',
                   message = 'subscribe(group, function) required',
                   retryable = false}
    end
    -- Attach first: a new group starts at the head (no retro delivery);
    -- a known cursor catches up below.
    prim.broadcast_attach(name, group)
    q._subs[group] = {fn = fn}
    -- One offline catch-up pass. Pushes racing inside the replay (from a
    -- re-entrant callback) already delivered through q:push above.
    local rows, last = prim.broadcast_since(name, group)
    if last > 0 then
      for i = 1, #rows do
        local ok, err = pcall(fn, prim.decode(rows[i]))
        if not ok then
          shield.log.warn('broadcast replay failed: ' .. tostring(err))
        end
      end
      prim.broadcast_commit(name, group, last)
    end
    return true
  end
  function q:unsubscribe(group)
    q._subs[group] = nil
    return true
  end
  function q:history() return prim.broadcast_history_size(name) end
  function q:groups() return prim.broadcast_group_count(name) end
  function q:purge()
    prim.broadcast_purge(name)
    return true
  end
  return q
end

-- ---- rate limiter bounded wait (polls allow like queue pops) ----
impl.attach_rate_wait = function(limiter)
  limiter.wait = function(self, key, timeout)
    local ok = wait_for(function() return self:allow(key) end, timeout, 10)
    if ok then return true end
    return false, timeout_error('rate limit wait', timeout or 0)
  end
  return limiter
end

-- ---- scheduler callback registry ----
impl.attach_sched = function(M, task, fn)
  impl.sched[task] = fn
  if not impl.sched_installed then
    impl.sched_installed = true
    local prev = rawget(M, 'on_scheduler_task')
    if type(prev) == 'function' then impl.sched_prev = prev end
    M.on_scheduler_task = function(ctx, task)
      if impl.sched_prev then
        local ok, err = pcall(impl.sched_prev, ctx, task)
        if not ok then
          shield.log.warn('on_scheduler_task failed: ' .. tostring(err))
        end
      end
      local fn2 = impl.sched[task]
      if fn2 then
        local ok2, err2 = pcall(fn2, ctx)
        if not ok2 then
          shield.log.warn('scheduler task failed: ' .. tostring(err2))
        end
      else
        shield.log.warn('scheduler task has no callback: ' .. tostring(task))
      end
    end
  end
  return true
end
impl.detach_sched = function(task) impl.sched[task] = nil return true end
impl.sched_count = function()
  local n = 0
  for _ in pairs(impl.sched) do n = n + 1 end
  return n
end

local api = {
  make_mutex = function(name, opts) return make_exclusive(name, opts, 'mutex') end,
  make_spinlock = function(name, opts)
    return make_exclusive(name, opts, 'spinlock')
  end,
  make_rwlock = make_rwlock,
  make_queue = make_queue,
  make_delay_queue = make_delay_queue,
  make_priority_queue = make_priority_queue,
  make_broadcast_queue = make_broadcast_queue,
  make_reliable_queue = make_reliable_queue,
  pop_with_wait = pop_with_wait,
  attach_rate_wait = impl.attach_rate_wait,
  attach_sched = impl.attach_sched,
  detach_sched = impl.detach_sched,
  sched_count = impl.sched_count,
}
rawset(_G, '__shield_global_impl', api)
return api
)lua";

void register_global_api(shd::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    shd::state_view lua(shield.lua_state());

    // Non-blocking primitives the orchestration chunk builds the waiting
    // API on. Created per VM, then handed to the chunk; C++ never keeps a
    // reference past this registration (call-time resolution only).
    shd::table prim = lua.create_table();
    auto* gm = shield::global::GlobalManager::global();
    auto lock_try = [gm](const std::string& registry, const std::string& name,
                         const std::string& owner, double ttl) {
        return gm->mutex_acquire(registry, name, owner,
                                 static_cast<std::uint64_t>(ttl)) ==
               shield::global::LockStatus::kOk;
    };
    prim.set_function("lock_try", lock_try);
    prim.set_function("lock_release",
                      [gm](const std::string& registry, const std::string& name,
                           const std::string& owner) {
                          return gm->mutex_release(registry, name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("lock_extend",
                      [gm](const std::string& registry, const std::string& name,
                           const std::string& owner, double ttl) {
                          return gm->mutex_extend(
                              registry, name, owner,
                              static_cast<std::uint64_t>(ttl));
                      });
    prim.set_function(
        "lock_info",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& registry, const std::string& name) {
            shd::state_view s(state);
            const auto info = gm->mutex_info(registry, name);
            if (!info.exists) {
                return shd::make_object(s, shd::nil);
            }
            shd::table out = s.create_table();
            out["exists"] = true;
            out["owner"] = info.owner;
            out["count"] = info.count;
            out["acquired_at"] = info.acquired_at_ms;
            out["ttl_remaining"] = info.ttl_remaining_ms;
            return shd::make_object(s, out);
        });
    prim.set_function(
        "rw_write_try",
        [gm](const std::string& name, const std::string& owner, double ttl) {
            return gm->rw_write_acquire(name, owner,
                                        static_cast<std::uint64_t>(ttl)) ==
                   shield::global::LockStatus::kOk;
        });
    prim.set_function("rw_write_release",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_write_release(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function(
        "rw_write_extend",
        [gm](const std::string& name, const std::string& owner, double ttl) {
            return gm->rw_write_extend(name, owner,
                                       static_cast<std::uint64_t>(ttl));
        });
    prim.set_function("rw_read_try",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_read_acquire(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("rw_read_release",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_read_release(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("queue_push", [gm](const std::string& name,
                                         const std::string& payload) {
        gm->queue_push(name, payload);
    });
    prim.set_function(
        "queue_pop_now",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> shd::object {
            shd::state_view s(state);
            std::string payload;
            if (!gm->queue_pop(name, &payload)) {
                return shd::make_object(s, shd::nil);
            }
            return shd::make_object(s, payload);
        });
    prim.set_function("queue_length", [gm](const std::string& name) {
        return gm->queue_length(name);
    });
    prim.set_function("queue_purge",
                      [gm](const std::string& name) { gm->queue_purge(name); });
    prim.set_function(
        "delay_push", [gm](const std::string& name, const std::string& payload,
                           double delay) {
            gm->delay_push(name, payload, static_cast<std::uint64_t>(delay));
        });
    prim.set_function(
        "delay_push_at",
        [gm](const std::string& name, const std::string& payload, double at) {
            gm->delay_push_at(name, payload, static_cast<std::uint64_t>(at));
        });
    prim.set_function(
        "delay_pop_now",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> shd::object {
            shd::state_view s(state);
            std::string payload;
            if (!gm->delay_pop(name, &payload)) {
                return shd::make_object(s, shd::nil);
            }
            return shd::make_object(s, payload);
        });
    prim.set_function("delay_pending", [gm](const std::string& name) {
        return gm->delay_pending(name);
    });
    prim.set_function("delay_ready", [gm](const std::string& name) {
        return gm->delay_ready(name);
    });
    prim.set_function("delay_purge",
                      [gm](const std::string& name) { gm->delay_purge(name); });
    prim.set_function("priority_push", [gm](const std::string& name,
                                            const std::string& payload,
                                            double priority) {
        gm->priority_push(name, payload, static_cast<std::int64_t>(priority));
    });
    prim.set_function(
        "priority_pop_now",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> shd::object {
            shd::state_view s(state);
            std::string payload;
            if (!gm->priority_pop(name, &payload)) {
                return shd::make_object(s, shd::nil);
            }
            return shd::make_object(s, payload);
        });
    prim.set_function("priority_length", [gm](const std::string& name) {
        return gm->priority_length(name);
    });
    prim.set_function("priority_purge", [gm](const std::string& name) {
        gm->priority_purge(name);
    });
    prim.set_function("broadcast_configure", [gm](const std::string& name,
                                                  double max_history) {
        gm->broadcast_configure(name, static_cast<std::size_t>(max_history));
    });
    prim.set_function("broadcast_push", [gm](const std::string& name,
                                             const std::string& payload) {
        return gm->broadcast_push(name, payload);
    });
    prim.set_function("broadcast_attach",
                      [gm](const std::string& name, const std::string& group) {
                          return gm->broadcast_attach(name, group);
                      });
    prim.set_function(
        "broadcast_since",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name,
             const std::string& group) -> std::tuple<shd::object, shd::object> {
            shd::state_view s(state);
            std::vector<std::string> rows;
            const std::uint64_t last = gm->broadcast_since(name, group, &rows);
            shd::table out = s.create_table();
            for (std::size_t i = 0; i < rows.size(); ++i) {
                out[i + 1] = rows[i];
            }
            return {shd::make_object(s, out), shd::make_object(s, last)};
        });
    prim.set_function(
        "broadcast_commit",
        [gm](const std::string& name, const std::string& group, double seq) {
            gm->broadcast_commit(name, group, static_cast<std::uint64_t>(seq));
        });
    prim.set_function("broadcast_history_size", [gm](const std::string& name) {
        return gm->broadcast_history_size(name);
    });
    prim.set_function("broadcast_group_count", [gm](const std::string& name) {
        return gm->broadcast_group_count(name);
    });
    prim.set_function("broadcast_purge", [gm](const std::string& name) {
        gm->broadcast_purge(name);
    });
    prim.set_function(
        "rel_configure", [gm](const std::string& name, double max_retries) {
            gm->reliable_configure(name, static_cast<int>(max_retries));
        });
    prim.set_function(
        "rel_push", [gm](const std::string& name, const std::string& payload) {
            gm->reliable_push(name, payload);
        });
    prim.set_function(
        "rel_pop_now",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> std::tuple<shd::object, shd::object> {
            shd::state_view s(state);
            shield::global::ReliableDelivery delivery;
            if (!gm->reliable_pop(name, &delivery)) {
                return {shd::make_object(s, shd::nil),
                        shd::make_object(s, shd::nil)};
            }
            return {shd::make_object(s, delivery.delivery_id),
                    shd::make_object(s, delivery.payload)};
        });
    prim.set_function("rel_ack", [gm](const std::string& name,
                                      double delivery_id) {
        return gm->reliable_ack(name, static_cast<std::uint64_t>(delivery_id));
    });
    prim.set_function("rel_nack", [gm](const std::string& name,
                                       double delivery_id, double retry) {
        switch (  // GCOVR_EXCL_BR_LINE (no-match arc; every NackResult value
                  // is listed in the case labels)
            gm->reliable_nack(  // GCOVR_EXCL_BR_LINE (no-match arc; every
                                // NackResult value is listed in the case
                                // labels)
                name, static_cast<std::uint64_t>(delivery_id),
                static_cast<std::uint64_t>(retry))) {
            case shield::global::NackResult::kRequeued:
                return "requeued";
            case shield::global::NackResult::kDead:
                return "dead";
            case shield::global::NackResult::kNotFound:
                break;
        }
        return "not_found";
    });
    prim.set_function(
        "rel_dead_range",
        [gm](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name, double from, double to) -> shd::object {
            shd::state_view s(state);
            auto entries =
                gm->reliable_dead_range(name, static_cast<std::size_t>(from),
                                        static_cast<std::size_t>(to));
            shd::table out = s.create_table();
            for (const auto& entry : entries) {
                shd::table row = s.create_table();
                row["id"] = entry.delivery_id;
                row["payload"] = entry.payload;
                row["retries"] = entry.retries;
                out.add(row);
            }
            return out;
        });
    prim.set_function("rel_dead_size", [gm](const std::string& name) {
        return gm->reliable_dead_size(name);
    });
    prim.set_function("rel_dead_purge", [gm](const std::string& name) {
        gm->reliable_dead_purge(name);
    });
    prim.set_function(
        "encode", [](shd::object value) { return lua_to_json(value).dump(); });
    prim.set_function(
        "decode",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           const std::string& text) -> shd::object {
            shd::state_view s(state);
            auto parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded()) {
                return shd::make_object(s, shd::nil);
            }
            return json_to_lua(s, parsed);
        });
    lua["__shield_global_primitives"] = prim;

    // Run the orchestration chunk once per VM.
    shd::safe_script(
        lua, kGlobalOrchestration,
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE

    // Resolve the impl table at CALL time (never captured: same rule as
    // the server orchestration chunk).
    auto impl_table = [lua]() {
        shd::table impl = lua.globals()["__shield_global_impl"];
        return impl;
    };

    // ---- shield.global(): global data + local cache ----
    shield.set_function(
        "global", [lua](shd::this_state state) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            shd::table g = s.create_table();
            g.set_function("set", [mgr](shd::object /*self*/, shd::object key,
                                        shd::object value,
                                        std::optional<double> ttl) {
                mgr->data_set(key.as<std::string>(), lua_to_json(value).dump(),
                              ttl ? static_cast<std::uint64_t>(*ttl) : 0);
                return true;
            });
            g.set_function(
                "get",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/, shd::object key) -> shd::object {
                    shd::state_view s(state);
                    std::string value;
                    if (!mgr->data_get(key.as<std::string>(), &value)) {
                        return shd::make_object(s, shd::nil);
                    }
                    auto parsed = nlohmann::json::parse(value, nullptr, false);
                    if (parsed.is_discarded()) {
                        return shd::make_object(s, value);
                    }
                    return json_to_lua(s, parsed);
                });
            g.set_function("delete", [mgr](shd::object, shd::object key) {
                return mgr->data_delete(key.as<std::string>());
            });
            g.set_function(
                "incr",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/, shd::object key,
                      std::optional<double> delta) -> shd::variadic_results {
                    shd::state_view s(state);
                    shd::variadic_results results;
                    std::int64_t out = 0;
                    std::string error;
                    if (!mgr->data_incr_by(
                            key.as<std::string>(),
                            delta ? static_cast<std::int64_t>(*delta) : 1, &out,
                            &error)) {
                        results.push_back(shd::make_object(s, shd::nil));
                        results.push_back(
                            make_error(state, "invalid_value", error));
                        return results;
                    }
                    results.push_back(shd::make_object(s, out));
                    return results;
                });
            g.set_function(
                "decr",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/, shd::object key,
                      std::optional<double> delta) -> shd::variadic_results {
                    shd::state_view s(state);
                    shd::variadic_results results;
                    std::int64_t out = 0;
                    std::string error;
                    if (!mgr->data_incr_by(
                            key.as<std::string>(),
                            delta ? -static_cast<std::int64_t>(*delta) : -1,
                            &out, &error)) {
                        results.push_back(shd::make_object(s, shd::nil));
                        results.push_back(
                            make_error(state, "invalid_value", error));
                        return results;
                    }
                    results.push_back(shd::make_object(s, out));
                    return results;
                });
            g.set_function(
                "mset",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/, shd::table kvs,
                      std::optional<double> ttl) -> shd::variadic_results {
                    shd::state_view s(state);
                    shd::variadic_results results;
                    std::vector<std::pair<std::string, std::string>> pairs;
                    for (auto&& [k, v] : kvs) {
                        if (k.is<std::string>()) {
                            pairs.emplace_back(k.as<std::string>(),
                                               lua_to_json(v).dump());
                        }
                    }
                    std::string error;
                    if (!mgr->data_mset(
                            pairs, ttl ? static_cast<std::uint64_t>(*ttl) : 0,
                            &error)) {
                        results.push_back(shd::make_object(s, shd::nil));
                        results.push_back(
                            make_error(state, "invalid_argument", error));
                        return results;
                    }
                    results.push_back(shd::make_object(s, true));
                    return results;
                });
            g.set_function(
                "mget",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/,
                      shd::variadic_args args) -> shd::object {
                    shd::state_view s(state);
                    shd::table out = s.create_table();
                    for (const auto& arg : args) {
                        std::string value;
                        if (mgr->data_get(arg.as<std::string>(), &value)) {
                            auto parsed =
                                nlohmann::json::parse(value, nullptr, false);
                            if (!parsed.is_discarded()) {
                                out.add(json_to_lua(s, parsed));
                                continue;
                            }
                            out.add(shd::make_object(s, value));
                        } else {
                            out.add(shd::make_object(s, shd::nil));
                        }
                    }
                    return out;
                });
            g.set_function(
                "get_cached",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object /*self*/, shd::object key,
                      std::optional<double> ttl) -> shd::object {
                    shd::state_view s(state);
                    std::string value;
                    if (!mgr->cache_get(
                            key.as<std::string>(),
                            ttl ? static_cast<std::uint64_t>(*ttl) : 0,
                            &value)) {
                        return shd::make_object(s, shd::nil);
                    }
                    auto parsed = nlohmann::json::parse(value, nullptr, false);
                    if (parsed.is_discarded()) {
                        return shd::make_object(s, value);
                    }
                    return json_to_lua(s, parsed);
                });
            g.set_function("invalidate", [mgr](shd::object, shd::object key) {
                mgr->cache_invalidate(key.as<std::string>());
                return true;
            });
            results.push_back(shd::make_object(s, g));
            return results;
        });

    // ---- lock factories (waiting semantics live in the chunk) ----
    auto lock_factory = [lua, impl_table](shd::this_state state,
                                          const char* maker, shd::object name,
                                          std::optional<shd::table> opts) {
        shd::state_view s(state);
        auto* mgr = shield::global::GlobalManager::global();
        if (mgr == nullptr) {
            return std::make_tuple(
                shd::make_object(s, shd::nil),
                shd::make_object(
                    s, make_error(state, "module_unavailable",
                                  "shield_global is not initialized")));
        }
        (void)mgr;
        shd::table impl = impl_table();
        shd::protected_function make = impl[maker];
        shd::protected_function_result result =
            name.is<std::string>() && opts.has_value()
                ? make(name.as<std::string>(), *opts)  // GCOVR_EXCL_BR_LINE
                                                       // (compiler artifact:
                                                       // shd as<> dispatch
                                                       // clones)
                : make(name.is<std::string>()  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: shd as<> dispatch
                                               // clones)
                           ? name.as<std::string>()  // GCOVR_EXCL_BR_LINE
                                                     // (compiler artifact: shd
                                                     // as<> bad_cast dispatch
                                                     // clones)
                           : "");  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // shd as<> dispatch clones)
        if (!result.valid()) {
            return std::make_tuple(
                shd::make_object(s, shd::nil),
                shd::make_object(s, make_error(state, "invalid_argument",
                                               "invalid lock arguments")));
        }
        // Unwrap the pfr explicitly: make_object on the whole result does
        // not reliably push the callee's first return value here.
        shd::object lock_obj = result.get<shd::object>(0);
        return std::make_tuple(std::move(lock_obj),
                               shd::make_object(s, shd::nil));
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda/function-close line)
    shield.set_function(
        "mutex", [lock_factory](shd::this_state state, shd::object name,
                                std::optional<shd::table> opts) {
            return lock_factory(state, "make_mutex", name, opts);
        });
    shield.set_function(
        "spinlock", [lock_factory](shd::this_state state, shd::object name,
                                   std::optional<shd::table> opts) {
            return lock_factory(state, "make_spinlock", name, opts);
        });
    shield.set_function(
        "rwlock", [lock_factory](shd::this_state state, shd::object name,
                                 std::optional<shd::table> opts) {
            return lock_factory(state, "make_rwlock", name, opts);
        });
    shield.set_function("distributed_mutex",
                        [lock_factory](shd::this_state state, shd::object name,
                                       std::optional<shd::table> opts) {
                            // P0: the distributed twin rides the in-process
                            // backend (the cross-process seam is the manager
                            // itself).
                            return lock_factory(state, "make_mutex", name,
                                                opts);
                        });
    shield.set_function("distributed_rwlock",
                        [lock_factory](shd::this_state state, shd::object name,
                                       std::optional<shd::table> opts) {
                            return lock_factory(state, "make_rwlock", name,
                                                opts);
                        });

    // ---- shield.rank(name) ----
    shield.set_function(
        "rank",
        [lua](shd::this_state state,
              shd::object board_obj) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            if (!board_obj.is<std::string>()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "invalid_argument",
                                             "rank requires a board name"));
                return results;
            }
            const std::string board = board_obj.as<std::string>();
            shd::table rank = s.create_table();
            rank.set_function(
                "update", [mgr, board](shd::object /*self*/, std::string uid,
                                       double score) {
                    mgr->rank_update(board, uid, score);
                    return true;
                });
            rank.set_function("mupdate", [mgr, board](shd::object /*self*/,
                                                      shd::table updates) {
                std::vector<std::pair<std::string, double>> batch;
                for (auto&& [uid, score] : updates) {
                    if (uid.is<std::string>() && score.is<double>()) {
                        batch.emplace_back(uid.as<std::string>(),
                                           score.as<double>());
                    }
                }
                mgr->rank_mupdate(board, batch);
                return true;
            });
            rank.set_function(
                "score",
                [mgr, board](shd::this_state state, shd::object /*self*/,
                             shd::object uid) -> shd::object {
                    shd::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return shd::make_object(s, shd::nil);
                    }
                    auto value = mgr->rank_score(board, uid.as<std::string>());
                    return value ? shd::make_object(s, *value)
                                 : shd::make_object(s, shd::nil);
                });
            rank.set_function(
                "position",
                [mgr, board](shd::this_state state, shd::object /*self*/,
                             shd::object uid) -> shd::object {
                    shd::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return shd::make_object(s, shd::nil);
                    }
                    auto value =
                        mgr->rank_position(board, uid.as<std::string>());
                    return value ? shd::make_object(s, *value)
                                 : shd::make_object(s, shd::nil);
                });
            auto entry_table = [](shd::state_view s,
                                  const shield::global::RankEntry& entry) {
                shd::table row = s.create_table();
                row["uid"] = entry.uid;
                row["score"] = entry.score;
                row["rank"] = entry.rank;
                return row;
            };  // GCOVR_EXCL_LINE (fn-close artifact)
            rank.set_function(
                "top",
                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: lambda-entry clone
                                            // arcs)
                    shd::this_state
                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
                                // arcs at the set_function boundary)
                    shd::object /*self*/, double n) -> shd::object {
                    shd::state_view s(state);
                    shd::table out = s.create_table();
                    for (const auto& entry :
                         mgr->rank_top(board, static_cast<std::size_t>(n))) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "range",
                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: lambda-entry clone
                                            // arcs)
                    shd::this_state
                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
                                // arcs at the set_function boundary)
                    shd::object /*self*/, double from,
                    double to) -> shd::object {
                    shd::state_view s(state);
                    shd::table out = s.create_table();
                    for (const auto& entry : mgr->rank_range(
                             board, static_cast<std::uint64_t>(from),
                             static_cast<std::uint64_t>(to))) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "range_by_score",
                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: lambda-entry clone
                                            // arcs)
                    shd::this_state
                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
                                // arcs at the set_function boundary)
                    shd::object /*self*/, double lo, double hi) -> shd::object {
                    shd::state_view s(state);
                    shd::table out = s.create_table();
                    for (const auto& entry :
                         mgr->rank_range_by_score(board, lo, hi)) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "around",
                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: lambda-entry clone
                                            // arcs)
                    shd::this_state
                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
                                // arcs at the set_function boundary)
                    shd::object /*self*/, shd::object uid,
                    double n) -> shd::object {
                    shd::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return shd::make_object(s, shd::nil);
                    }
                    shd::table out = s.create_table();
                    const auto around =
                        mgr->rank_around(board, uid.as<std::string>(),
                                         static_cast<std::size_t>(n));
                    shd::table above = s.create_table();
                    for (const auto& entry : around.above) {
                        above.add(entry_table(s, entry));
                    }
                    shd::table below = s.create_table();
                    for (const auto& entry : around.below) {
                        below.add(entry_table(s, entry));
                    }
                    out["above"] = above;
                    out["target"] =
                        around.target
                            ? shd::make_object(
                                  s,
                                  entry_table(  // GCOVR_EXCL_BR_LINE (compiler
                                                // artifact: entry_table
                                                // construction cold clones)
                                      s,
                                      *around.target))  // GCOVR_EXCL_BR_LINE
                                                        // (compiler artifact:
                                                        // entry_table
                                                        // construction cold
                                                        // clone; both ternary
                                                        // arms driven)
                            : shd::make_object(s, shd::nil);
                    out["below"] = below;
                    return out;
                });
            rank.set_function("count", [mgr, board](shd::object /*self*/) {
                return mgr->rank_count(board);
            });
            rank.set_function(
                "remove", [mgr, board](shd::object /*self*/, std::string uid) {
                    return mgr->rank_remove(board, uid);
                });
            rank.set_function("clear", [mgr, board](shd::object /*self*/) {
                mgr->rank_clear(board);
                return true;
            });
            results.push_back(shd::make_object(s, rank));
            return results;
        });

    // ---- queue factories (waiting pop semantics live in the chunk) ----
    auto queue_factory = [lua, impl_table](shd::this_state state,
                                           const char* maker, shd::object name,
                                           std::optional<shd::table> opts) {
        shd::state_view s(state);
        auto* mgr = shield::global::GlobalManager::global();
        if (mgr == nullptr) {
            return std::make_tuple(
                shd::make_object(s, shd::nil),
                shd::make_object(
                    s, make_error(state, "module_unavailable",
                                  "shield_global is not initialized")));
        }
        (void)mgr;
        shd::table impl = impl_table();
        shd::protected_function make = impl[maker];
        shd::protected_function_result result =
            opts.has_value() ? make(name, *opts) : make(name);
        if (!result.valid()) {
            return std::make_tuple(
                shd::make_object(s, shd::nil),
                shd::make_object(s, make_error(state, "invalid_argument",
                                               "invalid queue arguments")));
        }
        // Unwrap the pfr explicitly: make_object on the whole result does
        // not reliably push the callee's first return value here.
        shd::object queue_obj = result.get<shd::object>(0);
        return std::make_tuple(std::move(queue_obj),
                               shd::make_object(s, shd::nil));
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda/function-close line)
    shield.set_function(
        "queue", [queue_factory](shd::this_state state, shd::object name,
                                 std::optional<shd::table> opts) {
            return queue_factory(state, "make_queue", name, opts);
        });
    shield.set_function(
        "delay_queue", [queue_factory](shd::this_state state, shd::object name,
                                       std::optional<shd::table> opts) {
            return queue_factory(state, "make_delay_queue", name, opts);
        });
    shield.set_function("reliable_queue",
                        [queue_factory](shd::this_state state, shd::object name,
                                        std::optional<shd::table> opts) {
                            return queue_factory(state, "make_reliable_queue",
                                                 name, opts);
                        });
    shield.set_function("priority_queue",
                        [queue_factory](shd::this_state state, shd::object name,
                                        std::optional<shd::table> opts) {
                            return queue_factory(state, "make_priority_queue",
                                                 name, opts);
                        });
    shield.set_function("broadcast_queue",
                        [queue_factory](shd::this_state state, shd::object name,
                                        std::optional<shd::table> opts) {
                            return queue_factory(state, "make_broadcast_queue",
                                                 name, opts);
                        });

    // ---- shield.scheduler() ----
    shield.set_function(
        "scheduler",
        [lua, manager,  // GCOVR_EXCL_BR_LINE (shd argument-conversion arcs at
                        // the set_function boundary)
         runtime](shd::this_state state) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            auto register_task = [lua, manager, runtime](
                                     shd::this_state state, const char* type,
                                     shd::object name, shd::object schedule,
                                     shd::object cb) -> shd::variadic_results {
                shd::state_view s(state);
                shd::variadic_results results;
                auto* mgr2 = shield::global::GlobalManager::global();
                if (!name.is<std::string>() ||
                    name.as<std::string>()  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: shd as<> bad_cast
                                            // dispatch clones)
                        .empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // name-guard cold clone; both compound
                                     // arms driven)
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(
                        make_error(state, "invalid_argument",
                                   "task name must be a non-empty string"));
                    return results;
                }
                if (!cb.is<shd::function>()) {
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(
                        make_error(state, "invalid_argument",
                                   "task expects a callback function"));
                    return results;
                }
                std::string schedule_text;
                if (schedule.is<std::string>()) {
                    schedule_text = schedule.as<std::string>();
                } else if (schedule.is<double>()) {
                    const double raw = schedule.as<double>();
                    if (raw < 0.0 || raw != std::floor(raw)) {
                        results.push_back(shd::make_object(s, shd::nil));
                        results.push_back(make_error(
                            state, "invalid_argument",
                            "schedule must be a non-negative integer ms"));
                        return results;
                    }
                    schedule_text =
                        std::to_string(static_cast<std::uint64_t>(raw));
                } else {
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "schedule must be a cron string or an ms number"));
                    return results;
                }
                const std::string service_id = manager->current_service_id();
                if (service_id.empty()) {
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "scheduler tasks require a service context (register "
                        "from on_init or a handler)"));
                    return results;
                }
                std::string error;
                if (!mgr2->sched_register(type, name.as<std::string>(),
                                          schedule_text, service_id, &error)) {
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(
                        make_error(state, "invalid_argument", error));
                    return results;
                }
                // Resolve the calling service's module table and store the
                // callback in the per-VM registry (same VM resolution as
                // the server watch facade).
                shd::table module_tbl = shd::nil;
                if (runtime) {  // GCOVR_EXCL_BR_LINE (defensive: runtime is
                                // always live inside a dispatch context)
                    auto vm = manager->current_service_vm();
                    // Defensive: the fallback below covers dispatch
                    // scopes that carry no vm; every current call site
                    // resolves directly, so it never runs.
                    // GCOVR_EXCL_START (defensive)
                    if (!vm) {
                        vm = manager->service_vm(service_id);
                    }
                    // GCOVR_EXCL_STOP
                    if (vm) {  // GCOVR_EXCL_BR_LINE (defensive: a null vm here
                               // means the service module is gone)
                        module_tbl = runtime->service_table(vm);
                    }
                }
                // Defensive: the context check above already rejects
                // registrations made outside a loaded service module,
                // so the resolved table is always valid here.
                // GCOVR_EXCL_START (defensive)
                if (!module_tbl.valid()) {
                    mgr2->sched_remove(name.as<std::string>());
                    results.push_back(shd::make_object(s, shd::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "scheduler tasks require a loaded service module"));
                    return results;
                }
                // GCOVR_EXCL_STOP
                shd::table impl = s.globals()["__shield_global_impl"];
                shd::protected_function attach = impl["attach_sched"];
                attach(module_tbl, name, cb);
                results.push_back(shd::make_object(s, true));
                return results;
            };  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda/function-close
                // line)
            shd::table sched = s.create_table();
            sched.set_function(
                "cron", [register_task](shd::this_state state,
                                        shd::object /*self*/, shd::object name,
                                        shd::object expr, shd::object cb,
                                        std::optional<shd::table> /*opts*/) {
                    return register_task(state, "cron", name, expr, cb);
                });
            sched.set_function(
                "interval",
                [register_task](shd::this_state state, shd::object /*self*/,
                                shd::object name, shd::object ms,
                                shd::object cb,
                                std::optional<shd::table> /*opts*/) {
                    return register_task(state, "interval", name, ms, cb);
                });
            sched.set_function(
                "once", [register_task](shd::this_state state,
                                        shd::object /*self*/, shd::object name,
                                        shd::object delay, shd::object cb,
                                        std::optional<shd::table> /*opts*/) {
                    return register_task(state, "once", name, delay, cb);
                });
            sched.set_function(
                "get",
                [mgr](shd::this_state state,  // GCOVR_EXCL_LINE
                      shd::object, std::string name) -> shd::object {
                    shd::state_view s(state);
                    auto info = mgr->sched_get(name);
                    if (!info.has_value()) {
                        return shd::make_object(s, shd::nil);
                    }
                    shd::table out = s.create_table();
                    out["name"] = info->name;
                    out["type"] = info->type;
                    out["schedule"] = info->schedule;
                    out["next_run"] = info->next_run_ms;
                    out["last_run"] = info->last_run_ms;
                    out["run_count"] = info->run_count;
                    out["status"] = info->paused ? "paused"
                                    : info->done ? "done"
                                                 : "active";
                    return out;
                });
            sched.set_function("pause", [mgr](shd::object, std::string name) {
                return mgr->sched_pause(name);
            });
            sched.set_function("resume", [mgr](shd::object, std::string name) {
                return mgr->sched_resume(name);
            });
            sched.set_function(
                "remove", [lua, mgr](  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // lambda-entry clone arcs)
                              shd::this_state state,
                              shd::object,  // GCOVR_EXCL_BR_LINE (shd
                                            // argument-conversion arcs at the
                                            // set_function boundary)
                              std::string name) {
                    mgr->sched_remove(name);
                    shd::state_view s(state);
                    shd::table impl = s.globals()["__shield_global_impl"];
                    shd::protected_function detach = impl["detach_sched"];
                    detach(name);
                    return true;
                });
            sched.set_function("trigger", [mgr](shd::object, std::string name) {
                return mgr->sched_trigger(name);
            });
            results.push_back(shd::make_object(s, sched));
            return results;
        });

    // ---- shield.rate_limiter(name, opts) ----
    shield.set_function(
        "rate_limiter",
        [lua, impl_table](  // GCOVR_EXCL_BR_LINE (shd argument-conversion arcs
                            // at the set_function boundary)
            shd::this_state state, shd::object name_obj,
            std::optional<shd::table> opts) -> shd::variadic_results {
            shd::state_view s(state);
            shd::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            if (!name_obj.is<std::string>()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(make_error(state, "invalid_argument",
                                             "rate_limiter requires a name"));
                return results;
            }
            const std::string name = name_obj.as<std::string>();
            shield::global::RateLimitConfig config;
            if (opts.has_value()) {
                shd::table o = *opts;
                config.rate = o.get_or("rate", 100.0);
                config.burst = o.get_or("burst", 200.0);
                config.sliding = o.get_or("sliding", false);
                config.window_ms =
                    static_cast<std::uint64_t>(o.get_or("window", 60000.0));
                config.max_requests =
                    static_cast<std::uint64_t>(o.get_or("max_requests", 100.0));
                config.max_keys =
                    static_cast<std::size_t>(o.get_or("max_keys", 65536.0));
            }
            mgr->rate_limit_configure(name, config);
            shd::table limiter = s.create_table();
            limiter["name"] = name;
            limiter.set_function(
                "allow", [mgr, name](shd::object /*self*/, std::string key) {
                    return mgr->rate_limit_allow(name, key, 1.0).allowed;
                });
            limiter.set_function("remaining", [mgr, name](shd::object /*self*/,
                                                          std::string key) {
                return mgr->rate_limit_remaining(name, key);
            });
            // Bounded wait lives in the chunk (coroutine-aware sleep).
            shd::table impl = impl_table();
            shd::protected_function attach = impl["attach_rate_wait"];
            auto attached = attach(limiter);
            if (!attached.valid()) {
                results.push_back(shd::make_object(s, shd::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "rate limiter wait attach failed"));
                return results;
            }
            results.push_back(shd::make_object(s, limiter));
            return results;
        });
}
#else
// Module compiled out: every shield_global factory reports
// module_unavailable instead of being a nil field.
void register_global_stub_api(shd::table& shield, shd::state_view lua) {
    auto unavailable = shd::safe_script(
        lua,
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_global is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    for (const char* name :
         {"global", "mutex", "rwlock", "spinlock", "distributed_mutex",
          "distributed_rwlock", "rank", "queue", "delay_queue",
          "priority_queue", "broadcast_queue", "reliable_queue", "scheduler",
          "rate_limiter"}) {
        shield[name] = unavailable;
    }
}
#endif

}  // namespace shield::lua
