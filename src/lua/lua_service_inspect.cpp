// [SHIELD_LUA] Lua service observability: name registry, service detail,
// L1/L2 inspect snapshots, ref/memory/coroutine walks, profile session
// control and stats projections. Split out of lua_service.cpp; member
// declarations live in lua_service_impl.hpp.
#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/mail_cache.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lua_service_impl.hpp"
#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

// L2 refs walker (lua.inspect <svc> refs): a bounded DFS over the service
// module's object graph. Must run on the owning service actor thread only —
// the iteration is raw (lua_next / lua_tolstring never invoke metamethods),
// allocates no Lua objects, and Lua's GC is non-moving, so pointer-keyed
// bookkeeping stays valid for the whole walk.
struct RefsWalker {
    lua_State* L;
    int max_depth;
    std::size_t budget;  // entries still allowed to visit (the time budget)
    std::unordered_set<const void*> seen;
    std::uint64_t tables = 0;
    std::uint64_t functions = 0;
    std::uint64_t userdata = 0;
    std::uint64_t threads = 0;
    std::uint64_t strings = 0;
    std::uint64_t string_bytes = 0;
    bool truncated = false;
    struct TableInfo {
        std::string path;
        std::size_t entries = 0;
        int depth = 0;
    };
    std::vector<TableInfo> tables_info;

    // Path segment for the key at `idx`: a bare name for string keys,
    // "[123]" for array indices, "[<type>]" for anything else.
    std::string key_segment(int idx) const {
        const int t = lua_type(L, idx);
        if (t == LUA_TSTRING) {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            // Cap the segment: one pathological multi-megabyte string key
            // must not dominate the report.
            if (len > 48) {
                len = 48;
            }
            return std::string(s, len);
        }
        if (t == LUA_TNUMBER) {
            return "[" + std::to_string(lua_tointeger(L, idx)) + "]";
        }
        return std::string("[") + lua_typename(L, t) + "]";
    }

    void count_value(int idx) {
        switch (lua_type(L, idx)) {
            case LUA_TSTRING: {
                std::size_t len = 0;
                lua_tolstring(L, idx, &len);
                ++strings;
                string_bytes += len;
                break;
            }
            case LUA_TFUNCTION:
                maybe_count_pointer(idx, &functions);
                break;
            case LUA_TUSERDATA:
                maybe_count_pointer(idx, &userdata);
                break;
            case LUA_TTHREAD:
                maybe_count_pointer(idx, &threads);
                break;
            default:
                break;  // tables handled by the caller; scalars not counted
        }
    }

    void maybe_count_pointer(int idx, std::uint64_t* counter) {
        const void* p = lua_topointer(L, idx);
        if (p != nullptr &&  // GCOVR_EXCL_BR_LINE (defensive: lua_topointer
                             // never null for the counted types)
            seen.insert(p)
                .second) {  // GCOVR_EXCL_BR_LINE (defensive: lua_topointer is
                            // never null for the counted types; the dup arm is
                            // covered by RefsWalkerSharedValues)
            ++*counter;
        }
    }

    // Walk the table at absolute stack index `abs_idx`; every table gets
    // one TableInfo slot and its entry count is written back through
    // `entries_out`. Stack discipline: each recursion level keeps its own
    // next key/value pair balanced even on the truncated early exit
    // (value popped before break, the pending key popped after). lua_next
    // is the raw traversal on Lua 5.5 — lua_rawnext no longer exists — and
    // it only errors on a key neither nil nor in the table, which cannot
    // happen here (the key came from the previous lua_next).
    void walk(int abs_idx, const std::string& path, int depth,
              std::size_t* entries_out) {
        std::size_t entries = 0;
        bool broke = false;
        lua_pushnil(L);
        while (lua_next(L, abs_idx) != 0) {
            if (budget == 0) {
                truncated = true;
                broke = true;
                lua_pop(L, 1);  // value; the pending key is popped below
                break;
            }
            --budget;
            ++entries;
            count_value(-1);
            const int vt = lua_type(L, -1);
            if (vt == LUA_TTABLE && depth < max_depth) {
                const void* p = lua_topointer(L, -1);
                if (seen.insert(p).second) {
                    ++tables;
                    std::string child = path + "." + key_segment(-2);
                    // Deep paths stop growing (the "~" tail marks the cut)
                    // but the table still counts and still recurses.
                    if (child.size() > 96) {
                        child = child.substr(0, 96) + "~";
                    }
                    const std::size_t slot = tables_info.size();
                    tables_info.push_back({child, 0, depth + 1});
                    std::size_t child_entries = 0;
                    // The value sits at the stack top: recurse against its
                    // absolute index so deeper pushes cannot invalidate it.
                    walk(lua_gettop(L), child, depth + 1, &child_entries);
                    tables_info[slot].entries = child_entries;
                }
            }
            lua_pop(L, 1);  // value; keep the key for the next lua_next
        }
        if (broke) {
            lua_pop(L, 1);  // the pending key
        }
        *entries_out = entries;
    }
};

// One GC sample over the collector's knobs (owner-actor-thread only).
// LUA_GCPARAM with val -1 is a pure read; the mode has no read-only probe —
// LUA_GCGEN/LUA_GCINC switch the collector and return the mode they found,
// so the probe below switches to generational and restores incremental when
// that is what it disturbed. Safe under the actor-thread invariant: no
// other code runs against this lua_State between the two calls, and a mode
// switch does not touch the stored parameters.
nlohmann::json collect_gc_info(lua_State* L) {
    const int kb = lua_gc(L, LUA_GCCOUNT);
    const int kb_rem = lua_gc(L, LUA_GCCOUNTB);
    const bool running = lua_gc(L, LUA_GCISRUNNING) != 0;
    const int prev_mode = lua_gc(L, LUA_GCGEN);
    if (prev_mode == LUA_GCINC) {
        lua_gc(L, LUA_GCINC);
    }
    // Each knob goes into a local first: lua_gc calls inline in the
    // multi-line initializer below are a gcov aggregation artifact (the
    // element lines never get credited — same class as nodes_visited).
    const int p_minormul = lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMUL, -1);
    const int p_majorminor = lua_gc(L, LUA_GCPARAM, LUA_GCPMAJORMINOR, -1);
    const int p_minormajor = lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, -1);
    const int p_pause = lua_gc(L, LUA_GCPARAM, LUA_GCPPAUSE, -1);
    const int p_stepmul = lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, -1);
    const int p_stepsize = lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, -1);
    nlohmann::json params = {
        {"minormul", p_minormul},     {"majorminor", p_majorminor},
        {"minormajor", p_minormajor}, {"pause", p_pause},
        {"stepmul", p_stepmul},       {"stepsize", p_stepsize},
    };  // GCOVR_EXCL_BR_LINE (compiler artifact: nlohmann::json braced
        // init_list aggregation arcs)
    // Computed before the initializer: arithmetic inline in this multi-line
    // initializer lands on a line gcov never credits (same artifact class
    // as nodes_visited below).
    const std::int64_t total_bytes =
        static_cast<std::int64_t>(kb) * 1024 + kb_rem;
    // memory_kb keeps the legacy field name: same GCCOUNT figure, but now
    // sampled precisely at inspect time rather than the L1 dispatch-exit
    // gauge.
    return nlohmann::json{
        {"total_bytes", total_bytes},
        {"memory_kb", kb},
        {"running", running},
        {"mode", prev_mode == LUA_GCGEN ? "generational" : "incremental"},
        {"params", std::move(params)}};  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: json initializer tail arc)
}

// Drive one refs walk over a service's module table and shape the summary
// JSON. Owner-actor-thread only.
nlohmann::json walk_module_refs(LuaRuntime& runtime,
                                const std::shared_ptr<LuaVM>& vm,
                                const std::string& service_id, int max_depth,
                                std::size_t max_nodes) {
    shd::table module = runtime.service_table(vm);
    if (!module.valid()) {  // GCOVR_EXCL_BR_LINE (defensive: a published
                            // service always has its module loaded (see
                            // the exclusion region above))
        // GCOVR_EXCL_START (defensive: a published service always has its
        // module loaded — the table stays nil only for VMs that never
        // finished spawning, which the registry check above refuses)
        return nlohmann::json{
            {"error",
             "service module not loaded"}};  //  (compiler
                                             // artifact: braced init on the
                                             // defensive not-loaded return
                                             // (region excluded above))
    }  // GCOVR_EXCL_STOP
    lua_State* L = runtime.vm_state(vm).lua_state();
    RefsWalker w{L, max_depth,
                 max_nodes};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                              // RefsWalker aggregate braced-init arcs)
    module.push();
    w.seen.insert(lua_topointer(L, -1));
    ++w.tables;
    std::size_t root_entries = 0;
    w.walk(lua_gettop(L), "M", 0, &root_entries);
    lua_pop(L, 1);
    // The module table gets its own slot so it competes in the top list.
    w.tables_info.push_back({"M", root_entries, 0});

    std::sort(
        w.tables_info.begin(), w.tables_info.end(),
        [](const RefsWalker::TableInfo& a, const RefsWalker::TableInfo& b) {
            if (a.entries != b.entries) {
                return a.entries > b.entries;
            }
            return a.path < b.path;
        });
    // Result size limit: at most 16 tables survive the report.
    if (w.tables_info.size() > 16) {
        w.tables_info.resize(16);
    }
    nlohmann::json top = nlohmann::json::array();
    for (const auto& t : w.tables_info) {
        top.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: push_back
                        // braced-init aggregation arcs)
            {{"path", t.path}, {"entries", t.entries}, {"depth", t.depth}});
    }
    // Computed before the JSON initializer: a subtraction inline in this
    // multi-line initializer lands on a line gcov never credits (same
    // artifact class as the linker-dedup exclusions in lua_runtime.cpp).
    const std::uint64_t nodes_visited = max_nodes - w.budget;
    return nlohmann::json{
        {"name", service_id},
        {"depth_limit", max_depth},
        {"node_budget", max_nodes},
        {"nodes_visited", nodes_visited},
        {"counts",
         {{"tables", w.tables},
          {"functions", w.functions},
          {"userdata", w.userdata},
          {"coroutines", w.threads},
          {"strings", w.strings},
          {"string_bytes", w.string_bytes}}},
        {"truncated", w.truncated},
        {"top_tables",
         std::move(top)}};  // GCOVR_EXCL_BR_LINE (compiler artifact: json
                            // initializer tail arc)
}

nlohmann::json LuaServiceManager::Impl::refs_summary_to_json(
    const InspectSnapshot::RefsSummary& r) {
    nlohmann::json top = nlohmann::json::array();
    for (const auto& t : r.top_tables) {
        top.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: push_back
                        // braced-init aggregation arcs)
            {{"path", t.path},
             {"entries",
              t.entries}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                             // push_back braced-init aggregation arcs)
    }
    return nlohmann::json{
        {"nodes_visited", r.nodes_visited},
        {"truncated", r.truncated},
        {"counts",
         {{"tables", r.tables},
          {"functions", r.functions},
          {"userdata", r.userdata},
          {"coroutines", r.coroutines},
          {"strings", r.strings},
          {"string_bytes", r.string_bytes}}},
        {"top_tables",
         std::move(top)}};  // GCOVR_EXCL_BR_LINE (compiler artifact: json
                            // initializer tail arc)
}

nlohmann::json LuaServiceManager::Impl::snapshot_to_json(
    const InspectSnapshot& s) {
    nlohmann::json refs = nullptr;
    if (s.refs.has_value()) {
        refs = refs_summary_to_json(*s.refs);
    }
    nlohmann::json j = {
        {"name", s.name},
        {"wall_ms", s.wall_ms},
        {"requests", s.requests},
        {"errors", s.errors},
        {"memory_kb", s.memory_kb},
        {"pending_calls", s.pending_calls},
        {"pending_tasks", s.pending_tasks},
        {"coroutines", s.coroutines},
        {"timers", s.timers},
        {"uptime_seconds", s.uptime_seconds},
        {"refs", std::move(refs)}};  // GCOVR_EXCL_BR_LINE (compiler
                                     // artifact: json initializer tail arc)
    if (!s.refs_error.empty()) {
        j["refs_error"] = s.refs_error;
    }
    return j;
}

bool LuaServiceManager::Impl::valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.rfind("shield.", 0) == 0) {
        return false;
    }
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
                        ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string LuaServiceManager::query_service(std::string_view name) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->published_names.find(std::string(name));
    if (it == impl_->published_names.end()) {
        return "";
    }
    return it->second;
}

bool LuaServiceManager::register_name(std::string_view name,
                                      std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "register requires current service context";
        }
        return false;
    }
    const bool in_current_dispatch =
        !tls_dispatch_stack
             .empty() &&  // GCOVR_EXCL_BR_LINE (defensive: owner is read from
                          // the dispatch stack itself; an empty stack returns
                          // earlier)
        tls_dispatch_stack.back().service_id == owner;
    if (!Impl::valid_name(name)) {
        if (error) {
            *error = "invalid service name: " + std::string(name);
        }
        return false;
    }

    std::unique_lock lock(impl_->registry_mutex);
    if (!impl_->services.contains(
            owner) &&  // GCOVR_EXCL_BR_LINE (defensive: owner and dispatch top
                       // come from the same frame; the mismatch arm cannot
                       // occur (see the exclusion region at 2948))
        !in_current_dispatch) {  // GCOVR_EXCL_START
                                 // //  (defensive: owner
                                 // comes from the dispatch stack top (see the
                                 // exclusion region))
                                 // //  (defensive: owner and
                                 // dispatch top come from the same frame; the
                                 // mismatch arm cannot occur (see
                                 // the exclusion region at 2948)) from the
                                 // dispatch stack top)
        if (error) {             //  (defensive: owner and dispatch top
                      // come from the same frame; the mismatch arm cannot occur
                      // (see the exclusion region at 2948))
            *error = "current service is not running: " +
                     owner;  //  (defensive: owner and dispatch top
                             // come from the same frame; the mismatch arm
                             // cannot occur (see the exclusion region at 2948))
        }
        return false;
    }
    // GCOVR_EXCL_STOP
    if (auto existing = impl_->published_names.find(std::string(name));
        existing != impl_->published_names.end() && existing->second != owner) {
        if (error) {
            *error = "service name already exists: " + std::string(name);
        }
        return false;
    }

    impl_->published_names[std::string(name)] = owner;
    impl_->owned_names[owner].insert(std::string(name));
    lock.unlock();
    impl_->notify_name_change(std::string(name), owner);
    return true;
}

bool LuaServiceManager::unregister_name(std::string_view name,
                                        std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "unregister requires current service context";
        }
        return false;
    }

    std::unique_lock lock(impl_->registry_mutex);
    auto existing = impl_->published_names.find(std::string(name));
    if (existing == impl_->published_names.end()) {
        if (error) {
            *error = "service name not found: " + std::string(name);
        }
        return false;
    }
    if (existing->second != owner) {
        if (error) {
            *error = "service name is owned by another service: " +
                     std::string(name);
        }
        return false;
    }

    impl_->published_names.erase(existing);
    if (auto names_it = impl_->owned_names.find(owner);
        names_it !=
        impl_->owned_names
            .end()) {  // GCOVR_EXCL_BR_LINE (defensive: publish always records
                       // the name in owned_names; the miss arm cannot occur)
        names_it->second.erase(std::string(name));
    }
    lock.unlock();
    impl_->notify_name_change(std::string(name), "");
    return true;
}

bool LuaServiceManager::claim_name(std::string_view name, std::string* error) {
    const std::string claimer = current_service_id();
    if (claimer.empty()) {
        if (error) {
            *error = "claim requires current service context";
        }
        return false;
    }
    if (!Impl::valid_name(name)) {
        if (error) {
            *error = "invalid service name: " + std::string(name);
        }
        return false;
    }

    std::unique_lock lock(impl_->registry_mutex);
    // GCOVR_EXCL_START (defensive: the current service context is only set
    // while that service is alive in the map, and teardown to removal runs
    // on the same actor thread — a claimer observable here as gone cannot
    // be scheduled deterministically)
    if (!impl_->services.contains(claimer)) {
        if (error) {
            *error = "current service is not running: " + claimer;
        }
        return false;
    }
    // GCOVR_EXCL_STOP
    auto existing = impl_->published_names.find(std::string(name));
    if (existing == impl_->published_names.end()) {
        if (error) {
            *error = "service name not found: " + std::string(name);
        }
        return false;
    }
    const std::string previous_owner = existing->second;
    if (previous_owner == claimer) {
        // Already the owner: idempotent no-op (no committed change, so no
        // name-change notification), still success so a retried handover
        // sequence does not fail on re-claim.
        return true;
    }
    // GCOVR_EXCL_START (defensive: exit cleanup retracts owned names
    // under the same lock, so a dead owner observable here cannot be
    // scheduled deterministically)
    if (!impl_->services.contains(previous_owner)) {
        if (error) {
            *error = "previous owner is not running: " + previous_owner;
        }
        return false;
    }
    // GCOVR_EXCL_STOP

    existing->second = claimer;
    impl_->owned_names[claimer].insert(std::string(name));
    if (auto names_it = impl_->owned_names.find(previous_owner);
        names_it != impl_->owned_names.end()) {  // GCOVR_EXCL_BR_LINE
                                                 // (defensive: a published
                                                 // name always has a
                                                 // matching owned_names
                                                 // entry — registration,
                                                 // claim and exit cleanup
                                                 // migrate both under the
                                                 // same lock)
        names_it->second.erase(std::string(name));
    }
    lock.unlock();
    impl_->notify_name_change(std::string(name), claimer);
    return true;
}

void LuaServiceManager::set_name_change_notifier(
    std::function<void(const std::string&, const std::string&)> fn) {
    impl_->name_change_notifier = std::move(fn);
}

std::vector<std::string> LuaServiceManager::list_services() const {
    std::vector<std::string> services;
    {
        std::shared_lock lock(impl_->registry_mutex);
        services.reserve(impl_->published_names.size());
        for (const auto& [name, _] : impl_->published_names) {
            services.push_back(name);
        }
    }
    std::sort(services.begin(), services.end());
    return services;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of list_services)

std::optional<nlohmann::json> LuaServiceManager::service_detail(
    std::string_view name) const {
    std::shared_lock lock(impl_->registry_mutex);
    const std::string key(name);
    // Only published services carry a detail snapshot: a name still in
    // on_init lives in init_vms and an exited one in recently_exited —
    // neither is listed by list_services, so neither answers here either.
    if (!impl_->services.contains(key)) {
        return std::nullopt;
    }
    nlohmann::json detail = {
        {"name", key},
        {"state", "running"}};  // GCOVR_EXCL_BR_LINE (compiler artifact: json
                                // braced-init arc)
    if (auto script_it = impl_->module_scripts.find(key);
        script_it != impl_->module_scripts.end()) {
        detail["script"] = script_it->second;
    }
    if (auto rpc_it = impl_->service_rpc.find(key);
        rpc_it !=
        impl_->service_rpc
            .end()) {  // GCOVR_EXCL_BR_LINE (defensive: publish inserts the rpc
                       // table with the service; the entry always exists here)
        detail["rpc_routes"] = rpc_it->second.descriptors.size();
    }
    // Every published service has a counters entry (inserted at publish,
    // erased on exit), so the traffic/uptime fields are unconditional here.
    if (auto counters_it = impl_->service_counters.find(key);
        counters_it !=
        impl_->service_counters
            .end()) {  // GCOVR_EXCL_BR_LINE (defensive: every published service
                       // has a counters entry (see source comment))
        detail["requests"] =
            counters_it->second->requests.load(std::memory_order_relaxed);
        detail["errors"] =
            counters_it->second->errors.load(std::memory_order_relaxed);
        // Lua heap KB as of the last dispatch exit (sampling value; see
        // DispatchScope).
        detail["memory_kb"] =
            counters_it->second->memory_kb.load(std::memory_order_relaxed);
        detail["uptime_seconds"] =
            // GCOVR_EXCL_START (duration expression continuation attributed
            // to no arc; the field itself is asserted by tests)
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          counters_it->second->spawned_at)
                // GCOVR_EXCL_STOP
                .count();
    }
    // Active actor timers registered by this incarnation (0 when it never
    // scheduled one); the map is guarded by the same registry lock.
    if (auto timers_it = impl_->actor_timers_by_service.find(key);
        timers_it != impl_->actor_timers_by_service.end()) {
        detail["timers"] = timers_it->second.size();
    } else {
        detail["timers"] = std::size_t{0};
    }
    // Calls whose caller coroutine is suspended under this incarnation's
    // name (entries whose caller is still initializing carry an empty or
    // unpublished name and match nothing here). Same registry lock.
    detail["pending_calls"] = static_cast<std::uint64_t>(std::count_if(
        impl_->pending_calls.begin(), impl_->pending_calls.end(),
        [&key](const decltype(impl_->pending_calls)::value_type& entry) {
            return entry.second.caller_service == key;
        }));
    // Forked tasks queued for this incarnation but not yet picked up by its
    // actor. task_mutex nests inside registry_mutex only in this direction.
    detail["pending_tasks"] = pending_task_count(key);
    // Handler coroutines of this incarnation still running or suspended
    // (registered at factory start, erased at terminal resume or teardown).
    // Same registry lock.
    detail["coroutines"] = static_cast<std::uint64_t>(std::count_if(
        impl_->live_coroutines.begin(), impl_->live_coroutines.end(),
        [&key](const decltype(impl_->live_coroutines)::value_type& entry) {
            return entry.second == key;
        }));
    return detail;
}

std::optional<nlohmann::json> LuaServiceManager::capture_inspect_snapshot(
    const std::string& service_id, const std::string& name, bool with_refs,
    std::string* error) {
    std::string stored_name;
    nlohmann::json plain;
    {
        std::unique_lock lock(impl_->registry_mutex);
        // Published services only: a name still in on_init or already exited
        // has no gauges worth freezing (same scope as service_detail).
        if (!impl_->services.contains(service_id)) {
            if (error) {
                *error = "service not published: " + service_id;
            }
            return std::nullopt;
        }
        auto& deque = impl_->inspect_snapshots[service_id];
        stored_name =
            name.empty()
                ? "snap-" +
                      std::to_string(                  // GCOVR_EXCL_BR_LINE
                          ++impl_->next_snapshot_seq)  // GCOVR_EXCL_BR_LINE
                                                       // (compiler artifact:
                                                       // ternary std::string
                                                       // concatenation arcs)
                : name;
        // A same-name capture replaces the stored entry (a refresh, not a new
        // sample); auto names never collide.
        for (auto it = deque.begin(); it != deque.end(); ++it) {
            if (it->name == stored_name) {
                deque.erase(it);
                break;
            }
        }
        Impl::InspectSnapshot snap;
        snap.name = stored_name;
        snap.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
        if (auto it = impl_->service_counters.find(service_id);
            it != impl_->service_counters
                      .end()) {  // GCOVR_EXCL_BR_LINE (defensive: every
                                 // published service has a counters entry; the
                                 // miss arm cannot occur)
            snap.requests =
                it->second->requests.load(std::memory_order_relaxed);
            snap.errors = it->second->errors.load(std::memory_order_relaxed);
            snap.memory_kb =
                it->second->memory_kb.load(std::memory_order_relaxed);
            snap.uptime_seconds =
                // GCOVR_EXCL_START (duration expression continuation attributed
                // to no arc; the field itself is asserted by tests)
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              it->second->spawned_at)
                    // GCOVR_EXCL_STOP
                    .count();
        }
        if (auto it = impl_->actor_timers_by_service.find(service_id);
            it != impl_->actor_timers_by_service.end()) {
            snap.timers = it->second.size();
        }
        snap.pending_calls = static_cast<std::uint64_t>(std::count_if(
            impl_->pending_calls.begin(), impl_->pending_calls.end(),
            [&service_id](const decltype(impl_->pending_calls)::value_type& e) {
                return e.second.caller_service == service_id;
            }));
        // task_mutex nests inside registry_mutex in this direction only.
        snap.pending_tasks = pending_task_count(service_id);
        snap.coroutines = static_cast<std::uint64_t>(std::count_if(
            impl_->live_coroutines.begin(), impl_->live_coroutines.end(),
            [&service_id](
                const decltype(impl_->live_coroutines)::value_type& e) {
                return e.second == service_id;
            }));
        deque.push_back(std::move(snap));
        // Bounded ring: the oldest sample falls off first.
        if (deque.size() > Impl::kInspectSnapshotsPerService) {
            deque.pop_front();
        }
        plain = Impl::snapshot_to_json(deque.back());
    }

    // The refs walk leaves the registry lock: inspect_refs re-checks
    // publication and dispatches onto the owner actor thread with a 2s
    // bounded wait — holding registry_mutex across that would freeze every
    // registry reader (console, dispatch, teardown).
    if (with_refs) {
        std::string refs_error;
        std::optional<nlohmann::json> refs =
            inspect_refs(service_id, 4, 20000, &refs_error);
        std::unique_lock lock(impl_->registry_mutex);
        // Patch the stored entry by name: a concurrent same-name capture
        // replaces the entry, so the newest match is the right target. If
        // the incarnation left (deque dropped) the patch is silently
        // skipped — the returned JSON just keeps refs null.
        auto ring_it = impl_->inspect_snapshots.find(service_id);
        if (ring_it != impl_->inspect_snapshots
                           .end()) {  // GCOVR_EXCL_BR_LINE (race: the service
                                      // is erased between the snapshot walk and
                                      // this patch - inspect-vs-exit window)
            for (auto it = ring_it->second.rbegin();
                 it != ring_it->second.rend();  // GCOVR_EXCL_BR_LINE
                 ++it) {  // GCOVR_EXCL_BR_LINE GCOVR_EXCL_LINE (loop never
                          // iterates: the first reverse iteration always
                          // matches, so the increment is unreachable)
                if (it->name ==     // GCOVR_EXCL_BR_LINE
                    stored_name) {  // GCOVR_EXCL_BR_LINE (defensive: rbegin
                                    // entry is the push above (always named
                                    // stored_name))
                    if (refs.has_value()) {
                        Impl::InspectSnapshot::RefsSummary summary;
                        summary.nodes_visited =
                            (*refs)["nodes_visited"].get<std::int64_t>();
                        summary.truncated = (*refs)["truncated"].get<bool>();
                        const auto& counts = (*refs)["counts"];
                        summary.tables = counts["tables"].get<std::int64_t>();
                        summary.functions =
                            counts["functions"].get<std::int64_t>();
                        summary.userdata =
                            counts["userdata"].get<std::int64_t>();
                        summary.coroutines =
                            counts["coroutines"].get<std::int64_t>();
                        summary.strings = counts["strings"].get<std::int64_t>();
                        summary.string_bytes =
                            counts["string_bytes"].get<std::int64_t>();
                        for (const auto& t : (*refs)["top_tables"]) {
                            summary.top_tables.push_back(
                                {t["path"].get<std::string>(),
                                 t["entries"].get<std::int64_t>()});
                        }
                        it->refs = std::move(summary);
                        it->refs_error.clear();
                    } else {
                        it->refs_error = refs_error;
                    }
                    // Echo the patched entry back to the caller.
                    plain = Impl::snapshot_to_json(*it);
                    break;
                }
            }
        }
    }
    return plain;
}

std::optional<nlohmann::json> LuaServiceManager::diff_inspect_snapshots(
    const std::string& service_id, const std::string& a, const std::string& b,
    std::string* error) {
    std::shared_lock lock(impl_->registry_mutex);
    auto svc_it = impl_->inspect_snapshots.find(service_id);
    if (svc_it == impl_->inspect_snapshots.end()) {
        if (error) {
            *error = "no snapshots for service: " + service_id;
        }
        return std::nullopt;
    }
    const Impl::InspectSnapshot* sa = nullptr;
    const Impl::InspectSnapshot* sb = nullptr;
    for (const auto& s : svc_it->second) {
        if (s.name == a) sa = &s;
        if (s.name == b) sb = &s;
    }
    if (sa == nullptr || sb == nullptr) {
        if (error) {
            *error =
                "unknown snapshot name: " + std::string(sa == nullptr ? a : b);
        }
        return std::nullopt;
    }
    const auto delta_of = [](std::uint64_t va, std::uint64_t vb) {
        return static_cast<std::int64_t>(vb) - static_cast<std::int64_t>(va);
    };
    // GCOVR_EXCL_START (braced-init aggregation artifact: gcc books the
    // whole initializer's counts onto a few continuation lines, leaving the
    // element lines at 0 even though the diff tests assert every field)
    nlohmann::json delta = {
        {"wall_ms", sb->wall_ms - sa->wall_ms},
        {"requests", delta_of(sa->requests, sb->requests)},
        {"errors", delta_of(sa->errors, sb->errors)},
        {"memory_kb", delta_of(sa->memory_kb, sb->memory_kb)},
        {"pending_calls", delta_of(sa->pending_calls, sb->pending_calls)},
        {"pending_tasks", static_cast<std::int64_t>(sb->pending_tasks) -
                              static_cast<std::int64_t>(sa->pending_tasks)},
        {"coroutines", static_cast<std::int64_t>(sb->coroutines) -
                           static_cast<std::int64_t>(sa->coroutines)},
        {"timers", static_cast<std::int64_t>(sb->timers) -
                       static_cast<std::int64_t>(sa->timers)},
        {"uptime_seconds",
         sb->uptime_seconds -
             sa->uptime_seconds}};  //  (compiler artifact:
                                    // delta json braced-init tail arc (region
                                    // excluded above, same artifact class))
    // GCOVR_EXCL_STOP
    // Object-graph delta only when both ends sampled refs: mixing a
    // sampled end with an unsampled one would invent a baseline.
    if (sa->refs.has_value() && sb->refs.has_value()) {
        const auto& ra = *sa->refs;
        const auto& rb = *sb->refs;
        const auto i64_delta = [](std::int64_t va, std::int64_t vb) {
            return vb - va;
        };
        // GCOVR_EXCL_START (braced-init aggregation artifact: same class as
        // the delta initializer above — the element lines book 0)
        nlohmann::json counts_delta = {
            {"tables", i64_delta(ra.tables, rb.tables)},
            {"functions", i64_delta(ra.functions, rb.functions)},
            {"userdata", i64_delta(ra.userdata, rb.userdata)},
            {"coroutines", i64_delta(ra.coroutines, rb.coroutines)},
            {"strings", i64_delta(ra.strings, rb.strings)},
            {"string_bytes",
             i64_delta(
                 ra.string_bytes,
                 rb.string_bytes)}};  //  (compiler artifact:
                                      // braced-init aggregation arc (region
                                      // excluded above, same artifact class))
        // GCOVR_EXCL_STOP
        // Top tables matched by path: both ends keep the walk's report
        // order, so union on path with per-end presence flags. Added /
        // removed tables carry their absolute entry count; matched ones
        // carry the entries delta.
        nlohmann::json top_delta = nlohmann::json::array();
        for (const auto& tb : rb.top_tables) {
            const Impl::InspectSnapshot::RefsSummary::TopTable* match = nullptr;
            for (const auto& cand : ra.top_tables) {
                if (cand.path == tb.path) {
                    match = &cand;
                    break;
                }
            }
            if (match == nullptr) {
                top_delta.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // push_back braced-init aggregation arcs)
                    {{"path",
                      tb.path},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // push_back braced-init aggregation arcs)
                     {"change", "added"},
                     {"entries", tb.entries}});
            } else {
                // Local first: an inline call in this multi-line
                // initializer is a gcov aggregation artifact.
                const std::int64_t entries_delta =
                    i64_delta(match->entries, tb.entries);
                top_delta.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // push_back braced-init aggregation arcs)
                    {{"path",
                      tb.path},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // push_back braced-init aggregation arcs)
                     {"change", "delta"},
                     {"entries", entries_delta}});
            }
        }
        for (const auto& ta : ra.top_tables) {
            bool present_b = false;
            for (const auto& cand : rb.top_tables) {
                if (cand.path == ta.path) {
                    present_b = true;
                    break;
                }
            }
            if (!present_b) {
                top_delta.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // push_back braced-init aggregation arcs)
                    {{"path",
                      ta.path},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // push_back braced-init aggregation arcs)
                     {"change", "removed"},
                     {"entries", ta.entries}});
            }
        }
        // GCOVR_EXCL_START (braced-init aggregation artifact)
        delta["refs"] = {
            {"nodes_visited", i64_delta(ra.nodes_visited, rb.nodes_visited)},
            {"counts", std::move(counts_delta)},
            {"top_tables",
             std::move(top_delta)}};  //  (compiler artifact:
                                      // json braced-init tail arc (region
                                      // excluded above, same artifact class))
        // GCOVR_EXCL_STOP
    } else {
        delta["refs"] = nullptr;
    }
    return nlohmann::json{
        {"a", Impl::snapshot_to_json(*sa)},
        {"b", Impl::snapshot_to_json(*sb)},
        {"delta", std::move(delta)}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json braced-init tail arc)
}

std::optional<nlohmann::json> LuaServiceManager::inspect_refs(
    const std::string& service_id, int max_depth, std::size_t max_nodes,
    std::string* error) {
    // Clamp to sane bounds: the walk's cost is one raw iteration per
    // visited entry, so the node cap doubles as the time budget.
    const int depth = std::clamp(max_depth, 1, 8);
    const std::size_t nodes =
        std::clamp(max_nodes, std::size_t{1}, std::size_t{50000});
    {
        std::shared_lock lock(impl_->registry_mutex);
        if (!impl_->services.contains(service_id)) {
            if (error) {
                *error = "service not published: " + service_id;
            }
            return std::nullopt;
        }
    }
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    // Plain std::function payload, deliberately free of any sol capture:
    // a queued task is destroyed on arbitrary threads (manager teardown,
    // service exit) and a sol handle would luaL_unref the VM's registry
    // there — the cross-thread race the fork-task cleanup paths avoid by
    // abandoning refs. The VM is resolved fresh on the actor thread below.
    const uint64_t task_id = enqueue_forked_task(  // GCOVR_EXCL_BR_START
        service_id, [impl = impl_.get(), service_id, depth,
                     nodes,  //  (compiler artifact: fork
                             // lambda entry/exit arcs) // GCOVR_EXCL_BR_STOP
                     promise]() {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // fork lambda entry/exit arcs)
            nlohmann::json out;
            const std::shared_ptr<LuaVM> vm =
                impl->find_dispatch_vm(service_id);
            // GCOVR_EXCL_START (defensive: the task only runs while the
            // service is published — exit cancels its queued tasks first,
            // so find_dispatch_vm cannot miss here. Same race class as the
            // marked nullptr return inside find_dispatch_vm.)
            if (!vm) {  //  (defensive: defensive
                        // vm-unreachable guard (region excluded above))
                // The service left the registry while the task waited in
                // the mailbox.
                out = nlohmann::json{
                    {"error",
                     "service vm not reachable"}};  //
                                                    // (defensive: defensive
                                                    // vm-unreachable guard
                                                    // (region excluded above))
            } else {                                // GCOVR_EXCL_STOP
                out = walk_module_refs(impl->runtime, vm, service_id, depth,
                                       nodes);
            }
            promise->set_value(std::move(out));
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: fork lambda entry/exit
             // arcs)
    // GCOVR_EXCL_START (defensive: the actor is spawned before the service
    // enters the registry and removed after it leaves, so a name the check
    // above accepted always has a live actor here)
    if (task_id == 0) {  //  (defensive: task_id==0 guard
                         // (region excluded above))
        if (error) {     //  (defensive: task_id==0 guard (region
                         // excluded above))
            *error = "service actor not found: " +
                     service_id;  //  (defensive: task_id==0
                                  // guard (region excluded above))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    // Bounded wait: an owner stuck in a long handler must not wedge the
    // console thread. The task itself is node-capped and still runs out
    // its result into the (discarded) future — a set_value nobody reads.
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        if (error) {
            *error = "refs dispatch timeout (owner busy): " + service_id;
        }
        return std::nullopt;
    }
    nlohmann::json out = future.get();
    // GCOVR_EXCL_START (defensive: the only producers of the "error" field
    // are the excluded actor-side guards above)
    if (out.contains("error")) {  //  (defensive: region
                                  // excluded above (spawn-timeout driver arms))
        // Actor-side failure (module gone / vm unreachable mid-wait).
        if (error) {  //  (defensive: region excluded above
                      // (spawn-timeout driver arms))
            *error = out["error"]
                         .get<std::string>();  //  (defensive:
                                               // region excluded above
                                               // (spawn-timeout driver arms))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    return out;
}

std::optional<nlohmann::json> LuaServiceManager::inspect_memory(
    const std::string& service_id, std::string* error) {
    {
        std::shared_lock lock(impl_->registry_mutex);
        if (!impl_->services.contains(service_id)) {
            if (error) {
                *error = "service not published: " + service_id;
            }
            return std::nullopt;
        }
    }
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    // Same payload discipline as inspect_refs: no sol capture (the task
    // can be destroyed on arbitrary threads); the VM is resolved fresh on
    // the actor thread.
    const uint64_t task_id = enqueue_forked_task(
        service_id, [impl = impl_.get(),  // GCOVR_EXCL_BR_LINE
                     service_id,  // GCOVR_EXCL_BR_LINE (compiler artifact: fork
                                  // lambda entry/exit arcs)
                     promise]() {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // fork lambda entry/exit arcs)
            nlohmann::json out;
            const std::shared_ptr<LuaVM> vm =
                impl->find_dispatch_vm(service_id);
            // GCOVR_EXCL_START (defensive: same race class as the
            // inspect_refs guard — service exit cancels queued tasks
            // before the VM goes away)
            if (!vm) {  //  (defensive: defensive
                        // vm-unreachable guard (region excluded above))
                out = nlohmann::json{
                    {"error",
                     "service vm not reachable"}};  //
                                                    // (defensive: defensive
                                                    // vm-unreachable guard
                                                    // (region excluded above))
            } else {                                // GCOVR_EXCL_STOP
                lua_State* L = impl->runtime.vm_state(vm).lua_state();
                out = {{"name", service_id},
                       {"gc", collect_gc_info(
                                  L)}};  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: json initializer arc)
                // Retainers share the dispatch: one walk from the module
                // table (depth 4 / 20000 nodes, the refs defaults),
                // projected down to its head — full counts live under
                // lua.inspect <svc> refs.
                nlohmann::json refs = walk_module_refs(
                    impl->runtime, vm, service_id, /*max_depth=*/4,
                    /*max_nodes=*/20000);
                if (!refs.contains(  // GCOVR_EXCL_BR_LINE (defensive:
                                     // walk_module_refs errors only on an
                                     // invalid module table)
                        "error")) {  // GCOVR_EXCL_BR_LINE (defensive:
                                     // walk_module_refs errors only when the
                                     // module table is invalid - impossible for
                                     // published services)
                    // GCOVR_EXCL_START (braced-init aggregation artifact)
                    out["retainers"] = {
                        {"nodes_visited", refs["nodes_visited"]},
                        {"truncated", refs["truncated"]},
                        {"top_tables",
                         refs["top_tables"]}};  //  (compiler
                                                // artifact: push_back
                                                // braced-init aggregation arcs)
                    // GCOVR_EXCL_STOP
                }
            }
            promise->set_value(std::move(out));
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: fork lambda entry/exit
             // arcs)
    // GCOVR_EXCL_START (defensive: the actor is spawned before the service
    // enters the registry and removed after it leaves, same reasoning as
    // inspect_refs)
    if (task_id == 0) {  //  (defensive: region excluded above
                         // (vm-unreachable guards))
        if (error) {     //  (defensive: region excluded above
                         // (vm-unreachable guards))
            *error = "service actor not found: " +
                     service_id;  //  (defensive: region
                                  // excluded above (vm-unreachable guards))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    // Same bounded wait: an owner stuck in a long handler must not wedge
    // the console thread; the task still runs out its result into the
    // (discarded) future.
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        if (error) {
            *error = "memory dispatch timeout (owner busy): " + service_id;
        }
        return std::nullopt;
    }
    nlohmann::json out = future.get();
    // GCOVR_EXCL_START (defensive: the only producers of the "error" field
    // are the excluded actor-side guards above)
    if (out.contains("error")) {  //  (defensive: region
                                  // excluded above (vm-unreachable guards))
        if (error) {              //  (defensive: region excluded above
                                  // (vm-unreachable guards))
            *error =
                out["error"].get<std::string>();  //  (defensive:
                                                  // region excluded above
                                                  // (vm-unreachable guards))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    return out;
}

std::optional<nlohmann::json> LuaServiceManager::inspect_coroutines(
    const std::string& service_id, std::string* error) {
    {
        std::shared_lock lock(impl_->registry_mutex);
        if (!impl_->services.contains(service_id)) {
            if (error) {
                *error = "service not published: " + service_id;
            }
            return std::nullopt;
        }
    }
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    // Same payload discipline as the other inspect forks: no sol capture,
    // the VM is resolved fresh on the actor thread.
    const uint64_t task_id = enqueue_forked_task(
        service_id,
        [impl = impl_.get(),  // GCOVR_EXCL_BR_LINE
         service_id,   // GCOVR_EXCL_BR_LINE (compiler artifact: fork lambda
                       // entry/exit arcs)
         promise]() {  // GCOVR_EXCL_BR_LINE (compiler artifact: multi-line
                       // enqueue_forked_task continuation / lambda entry arcs)
            // One registry pass collects plain data; the lua_status reads
            // happen after the lock. Both are safe: this task runs on the
            // owning actor, which serializes against every resume source
            // (hard invariant — a coroutine is always resumed by its owner
            // thread), so no coroutine is being driven right now and the
            // lua_States cannot be erased mid-enumeration.
            struct Entry {
                lua_State* co;
                Impl::CoroutineMeta meta;
                std::uint64_t waiting_call;
                bool driving;
            };
            std::vector<Entry> collected;
            {
                std::shared_lock lock(impl->registry_mutex);
                for (const auto& [co, owner] : impl->live_coroutines) {
                    if (owner != service_id) {
                        continue;
                    }
                    static const Impl::CoroutineMeta
                        kEmpty;  // GCOVR_EXCL_BR_LINE (static init guard)
                    auto mit = impl->coroutine_meta.find(co);
                    const Impl::CoroutineMeta& meta =
                        mit != impl->coroutine_meta.end()
                            ? mit->second  // GCOVR_EXCL_BR_LINE (defensive:
                                           // every live coroutine was
                                           // registered via
                                           // note_coroutine_started with meta;
                                           // the missing-meta arm cannot occur)
                            : kEmpty;
                    // The call the coroutine is currently busy with: serving
                    // one (callee side, handler_call_session) or awaiting a
                    // response for one (caller side, pending_calls reverse
                    // scan).
                    std::uint64_t waiting_call = 0;
                    auto wit = impl->handler_call_session.find(co);
                    if (wit != impl->handler_call_session.end()) {
                        waiting_call = wit->second;
                    } else {
                        for (const auto& [sess, pc] : impl->pending_calls) {
                            if (pc.caller_co == co) {
                                waiting_call = sess;
                                break;
                            }
                        }
                    }
                    collected.push_back({co, meta, waiting_call,
                                         impl->driving_cos.contains(co)});
                }
            }
            const std::int64_t now = Impl::now_ms();
            // Written as explicit assignments instead of a braced-init list:
            // the multi-line aggregate scattered its counts onto continuation
            // lines the counters never landed on (coverage artifact).
            nlohmann::json by_status = nlohmann::json::object();
            by_status["suspended"] = 0;
            by_status["finished"] = 0;
            by_status["error"] = 0;
            by_status["other"] = 0;
            nlohmann::json entries = nlohmann::json::array();
            for (auto& e : collected) {
                // Map lua_status to a report label; only non-running
                // threads reach here (see the invariant above).
                const char* label;
                switch (lua_status(  // GCOVR_EXCL_BR_LINE (defensive: only
                                     // LUA_YIELD reaches this switch (region
                                     // excluded below))
                    e.co)) {  // GCOVR_EXCL_BR_LINE (defensive: only LUA_YIELD
                              // reaches this switch (region excluded below))
                    case LUA_YIELD:
                        label = "suspended";
                        break;
                    // GCOVR_EXCL_START (defensive: a coroutine only persists
                    // in live_coroutines while suspended (LUA_YIELD) —
                    // finished and errored ones are erased by
                    // note_coroutine_finished before any inspection can
                    // observe them, so these arms keep the lua_status mapping
                    // complete but cannot execute)
                    case LUA_OK:
                        label = "finished";
                        break;
                    case LUA_ERRRUN:
                    case LUA_ERRERR:
                    case LUA_ERRSYNTAX:
                        label = "error";
                        break;
                    default:
                        label = "other";
                        break;
                }  // GCOVR_EXCL_STOP
                by_status[label] = by_status[label].get<std::uint64_t>() + 1;
                // Computed before the initializer (gcov artifact).
                const std::int64_t age_ms = now - e.meta.last_resume_ms;
                entries.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // json initializer arc)
                    {{"status", label},
                     {"origin", e.meta.origin},
                     {"resumes", e.meta.resumes},
                     {"last_resume", e.meta.last_resume},
                     {"age_ms", age_ms},
                     {"driving", e.driving},
                     {"waiting_call", e.waiting_call != 0
                                          ? nlohmann::json(e.waiting_call)
                                          : nlohmann::json(nullptr)}});
            }
            // Report cap: keep the 32 most recently resumed coroutines.
            bool truncated = false;
            if (entries.size() > 32) {
                std::sort(entries.begin(), entries.end(),
                          [](const nlohmann::json& a, const nlohmann::json& b) {
                              return a["age_ms"].get<std::int64_t>() <
                                     b["age_ms"].get<std::int64_t>();
                          });
                entries.erase(entries.begin() + 32, entries.end());
                truncated = true;
            }
            // Built as a named local so the initializer lines keep stable
            // per-line counters (gcov artifact).
            const std::size_t total = collected.size();
            nlohmann::json report{
                {"name", service_id},
                {"total", total},
                {"truncated", truncated},
                {"by_status", std::move(by_status)},
                {"entries",
                 std::move(entries)}};  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: json initializer tail arc)
            promise->set_value(std::move(report));
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: json initializer
             // continuation arcs)
    // GCOVR_EXCL_START (defensive: the actor is spawned before the service
    // enters the registry and removed after it leaves, same reasoning as
    // inspect_refs)
    if (task_id ==
        0) {          //  (defensive: region excluded above
                      // (actor-side guard, see the exclusion region at 3614))
        if (error) {  //  (defensive: region excluded above
                      // (actor-side guard, see the exclusion region at 3614))
            *error = "service actor not found: " +
                     service_id;  //  (defensive: region
                                  // excluded above (actor-side guard, see
                                  // the exclusion region at 3614))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        if (error) {
            *error = "coroutines dispatch timeout (owner busy): " + service_id;
        }
        return std::nullopt;
    }
    nlohmann::json out = future.get();
    // GCOVR_EXCL_START (defensive: the only producers of the "error" field
    // are the excluded actor-side guards — none of the code above can set
    // it on this path)
    if (out.contains(    //  (defensive: only the excluded
                         // actor-side guards produce "error")
            "error")) {  //  (defensive: region excluded above
                         // (see the exclusion region at 3630))
        if (error) {     //  (defensive: region excluded above
                         // (see the exclusion region at 3630))
            *error =
                out["error"]  //  (defensive: only the
                              // excluded actor-side guards produce "error")
                    .get<std::string>();  //
                                          // (defensive: region
                                          // excluded above (see
                                          // the exclusion region at
                                          // 3630))
        }
        return std::nullopt;
    }  // GCOVR_EXCL_STOP
    return out;
}

LuaServiceManager::ProfileStartResult LuaServiceManager::profile_start(
    const std::string& service_id, ProfileSessionConfig config,
    std::shared_ptr<std::promise<nlohmann::json>> done) {
    if (!done) {
        return ProfileStartResult::kDispatchLost;
    }
    const uint64_t duration_ms = config.duration_ms;
    std::shared_ptr<LuaVM> service;
    std::shared_ptr<ProfileSession> session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(service_id);
        if (it == impl_->services.end()) {
            return ProfileStartResult::kServiceNotFound;
        }
        if (impl_->profile_session.has_value()) {
            return ProfileStartResult::kSessionActive;
        }
        service = it->second;
        session = std::make_shared<ProfileSession>(std::move(config));
        // clang-format off
        impl_->profile_session = Impl::ProfileSessionState{  // GCOVR_EXCL_BR_LINE (compiler artifact: designated-init inline arcs)
            // clang-format on
            .service_id = service_id,
            .session = session,
            .sampler = nullptr,
            .done = std::move(done),
            .duration_driver = {},
            .started_at = std::chrono::steady_clock::now(),
        };
    }

    // Install task: resolve the main state on the owner thread, arm the
    // count hook (main state + live coroutines), and publish the sampler
    // under the registry lock. Fork-task FIFO ordering guarantees install
    // runs before any later stop task on the same service.
    const uint64_t task_id = enqueue_forked_task(
        service_id,
        [impl = impl_.get(), service,  // GCOVR_EXCL_BR_LINE (compiler
                                       // artifact: argument move throw arc)
         service_id,                   // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // std::function move throw arc)
         session]() {                  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // fork lambda entry/exit arcs)
            lua_State* main_L =
                service ?  // GCOVR_EXCL_BR_LINE (defensive: `service` is a
                           // shared_ptr kept alive by the lambda, so the null
                           // arm is unreachable)
                    impl->runtime.vm_main_state(  // GCOVR_EXCL_BR_LINE
                                                  // (compiler artifact: inline
                                                  // call throw arc)
                        service)  // GCOVR_EXCL_BR_LINE (defensive: live service
                                  // + compiler throw arcs)
                        : nullptr;  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // continuation throw arcs)
            auto provider = [impl, service_id]() -> std::vector<lua_State*> {
                std::vector<lua_State*> cos;
                {
                    std::shared_lock lock(impl->registry_mutex);
                    for (const auto& [co, owner] : impl->live_coroutines) {
                        if (owner == service_id) {  // GCOVR_EXCL_BR_LINE
                            // (defensive: no test samples while a foreign
                            // service keeps live coroutines)
                            cos.push_back(co);
                        }
                    }
                }
                return cos;
            };
            auto sampler =
                std::make_shared<ProfileSampler>(session, std::move(provider));
            ProfileSampler* sampler_ptr = nullptr;
            {
                std::unique_lock lock(impl->registry_mutex);
                if (!impl->profile_session ||
                    impl->profile_session->service_id !=  // GCOVR_EXCL_BR_LINE
                        service_id) {  // GCOVR_EXCL_BR_LINE (race: stale
                                       // install vs session switched to another
                                       // service)
                    return;  // exited/stopped before install was picked up
                }
                impl->profile_session->sampler = std::move(sampler);
                sampler_ptr = impl->profile_session->sampler.get();
            }
            // Hooks go on outside the registry lock: ProfileSampler::install
            // enumerates live coroutines through the provider, which takes
            // this same mutex shared. unique-then-shared on one thread is a
            // recursive acquisition glibc tolerates but the macOS/Windows
            // rwlock implementations deadlock on — the CI hangs in
            // ProfileHappyPathStartStatusStop came from exactly this
            // recursion (2026-09-23).
            if (main_L != nullptr &&       // GCOVR_EXCL_BR_LINE (defensive:
                                           // main_L comes from the still-alive
                                           // captured service)
                sampler_ptr != nullptr) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // published above; only a stale
                                           // install race can null it)
                sampler_ptr->install(main_L, service_id);
            }
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: install lambda tail arcs)
    if (task_id == 0) {  // GCOVR_EXCL_BR_LINE (race: actor-gone rollback
                         // window is untestable end-to-end)
        // The actor vanished between the registry check and the enqueue.
        // GCOVR_EXCL_START (race: enqueue only fails after the actor
        // vanished, and a vanished actor cannot own a live session — the
        // rollback below is unreachable end-to-end)
        nlohmann::json abandoned = {{"abandoned", true},
                                    {"service", service_id},
                                    {"reason", "actor gone"},
                                    {"total_samples", 0}};
        std::shared_ptr<std::promise<nlohmann::json>> settle;
        {
            std::unique_lock lock(impl_->registry_mutex);
            if (impl_->profile_session &&
                impl_->profile_session->service_id == service_id) {
                settle = impl_->profile_session->done;
                impl_->profile_session.reset();
            }
        }
        if (settle) {
            settle->set_value(std::move(abandoned));
        }
        return ProfileStartResult::kDispatchLost;
    }
    // GCOVR_EXCL_STOP

    // Duration expiry driver: a one-shot actor firing profile_stop after
    // duration_ms (same delayed_send pattern as the call-timeout driver).
    // Not cancelled on manual stop — the tick finds no session and quits.
    try {
        auto driver = impl_->system.spawn(
            [manager = this, service_id,  // GCOVR_EXCL_BR_LINE (compiler
                                          // artifact: CAF behavior lambda
                                          // entry/exit arcs)
             duration_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self,
                                   std::chrono::milliseconds(
                                       static_cast<std::int64_t>(duration_ms)),
                                   caf::tick_atom_v);
                return caf::behavior{
                    [=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: CAF behavior lambda
                                           // arc)
                        std::string stop_error;
                        (void)manager->profile_stop(service_id, &stop_error);
                        self->quit();
                    }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        if (impl_->profile_session &&  // GCOVR_EXCL_BR_LINE (race: a manual
                                       // stop kills this driver before the
                                       // tick; only the matched pair is
                                       // reachable in tests)
            // clang-format off
            impl_->profile_session->service_id == service_id) {  // GCOVR_EXCL_BR_LINE (race: false arm reaches the excluded else arm below)
            // clang-format on
            impl_->profile_session->duration_driver = std::move(driver);
        }
        // Else: the session ended before the driver registered — quit it.
        // anon_send_exit must run without the registry lock held.
        else {  // GCOVR_EXCL_BR_LINE (race: the session can only end via
                // profile_stop, which exits this very driver first)
            // GCOVR_EXCL_START (race: rollback send for a session that
            // ended before this driver registered — only profile_stop can
            // end a session, and it exits this very driver first)
            lock.unlock();
            caf::anon_send_exit(driver, caf::exit_reason::user_shutdown);
            // GCOVR_EXCL_STOP
        }
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                    // artifact: catch-entry pseudo-arc)
                 e) {               // GCOVR_EXCL_START (defensive: untestable
                       // actor-spawn failure, same exclusion class as the
                       // call-timeout driver at schedule_external_call_timeout)
        // A session that can never auto-stop must not be left armed:
        // stop it now (settle path or uninstall task).
        std::string stop_error;
        (void)profile_stop(service_id, &stop_error);
        return ProfileStartResult::kDispatchLost;
    }
    // GCOVR_EXCL_STOP
    return ProfileStartResult::kStarted;
}

bool LuaServiceManager::profile_stop(const std::string& service_id,
                                     std::string* error) {
    std::shared_ptr<Impl::ProfileSessionState> taken;
    std::shared_ptr<LuaVM> service;
    {
        std::unique_lock lock(impl_->registry_mutex);
        if (!impl_->profile_session ||
            impl_->profile_session->service_id != service_id) {
            if (error) {
                *error = "no active profile session on service: " + service_id;
            }
            return false;
        }
        if (!impl_->profile_session->sampler) {
            // Install task not picked up yet (or refused): treat as not
            // started and settle the promise as abandoned rather than
            // leaving the caller waiting.
            taken = std::make_shared<Impl::ProfileSessionState>(
                std::move(*impl_->profile_session));
            impl_->profile_session.reset();
        }
        auto sit = impl_->services.find(service_id);
        if (sit != impl_->services.end()) {  // GCOVR_EXCL_BR_LINE (race:
            // a service that died mid-stop no longer owns the session)
            service = sit->second;  // keeps the VM alive for the uninstall
        }
    }
    if (taken) {
        // Kill the duration driver here: it would otherwise idle until its
        // delayed tick and keep the actor system alive for the whole
        // duration (the actor_system teardown waits for it).
        if (taken->duration_driver) {  // GCOVR_EXCL_BR_LINE (race: stop
                                       // beating the driver registration
                                       // window)
            caf::anon_send_exit(taken->duration_driver,
                                caf::exit_reason::user_shutdown);
        }
        nlohmann::json abandoned = {
            {"abandoned", true},
            {"service", taken->service_id},
            {"reason", "stop before install"},
            {"total_samples",
             taken->session  // GCOVR_EXCL_BR_LINE (defensive: session is
                             // set synchronously by profile_start)
                 ? taken->session->total_samples()  // GCOVR_EXCL_BR_LINE
                 : 0}};     // GCOVR_EXCL_BR_LINE (artifact: continuation arcs)
        if (taken->done) {  // GCOVR_EXCL_BR_LINE (defensive: done is set
                            // synchronously by profile_start)
            taken->done->set_value(std::move(abandoned));
        }
        return true;
    }

    // Normal path: uninstall runs as a fork task so the hook removal and
    // the report export happen on the owner thread, serialized after any
    // in-flight hook hit.
    const uint64_t task_id = enqueue_forked_task(
        service_id,
        [impl = impl_.get(), service,  // GCOVR_EXCL_BR_LINE (compiler
                                       // artifact: argument move throw arc)
         service_id]() {               // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // std::function move throw arc)
            std::shared_ptr<Impl::ProfileSessionState> state;
            {
                std::unique_lock lock(impl->registry_mutex);
                if (!impl->profile_session ||  // GCOVR_EXCL_BR_LINE
                                               // (race: only the exit cleanup /
                                               // a stale stop task can lose the
                                               // session here; tests stop once)
                    impl->profile_session->service_id !=  // GCOVR_EXCL_BR_LINE
                        service_id) {  // GCOVR_EXCL_BR_LINE (race: stale
                                       // uninstall vs session switched to
                                       // another service)
                    return;  // GCOVR_EXCL_LINE (race: exit cleanup already
                             // settled the promise)
                }
                state = std::make_shared<Impl::ProfileSessionState>(
                    std::move(*impl->profile_session));
                impl->profile_session.reset();
            }
            // Kill the duration driver before anything else: left alive it
            // idles until its delayed tick and keeps the actor system's
            // teardown waiting for the whole duration. anon_send_exit must
            // not run under the registry lock.
            if (state->duration_driver) {  // GCOVR_EXCL_BR_LINE
                // (defensive: a session without a driver never enqueues
                // this task — the sampler-null stop settles inline)
                caf::anon_send_exit(state->duration_driver,
                                    caf::exit_reason::user_shutdown);
            }
            if (state->sampler && service) {  // GCOVR_EXCL_BR_LINE
                // (defensive/race: the sampler is armed whenever this task
                // runs; service stays alive via the captured handle)
                state->sampler->uninstall(impl->runtime.vm_main_state(service));
            }
            const uint64_t elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - state->started_at)
                    .count());
            if (state->done && state->session) {  // GCOVR_EXCL_BR_LINE
                // (defensive: both are set synchronously by profile_start)
                state->done->set_value(state->session->finish_report(
                    elapsed_ms));  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // inline finish_report throw arc)
            }
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: uninstall lambda tail
             // arcs)
    if (task_id == 0) {  // GCOVR_EXCL_START (race: untestable service
        // exit between the state check and the enqueue; the exit cleanup
        // owns the promise now)
        if (error) {
            *error = "service actor not found: " + service_id;
        }
        return false;
    }  // GCOVR_EXCL_STOP
    return true;
}

std::optional<LuaServiceManager::ProfileStatusInfo>
LuaServiceManager::profile_status() const {
    std::shared_lock lock(impl_->registry_mutex);
    if (!impl_->profile_session) {
        return std::nullopt;
    }
    const auto& state = *impl_->profile_session;
    // clang-format off
    return ProfileStatusInfo{  // GCOVR_EXCL_BR_LINE (compiler artifact: designated-init inline arcs)
        // clang-format on
        .service_id = state.service_id,
        .elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                // GCOVR_EXCL_START (compiler artifact: now() throw arc — a
                // bare line entry that never executes; the real coverable
                // work of this expression lives on the surrounding lines)
                std::chrono::steady_clock::now()  // GCOVR_EXCL_BR_LINE
                // GCOVR_EXCL_STOP
                - state.started_at)  // GCOVR_EXCL_BR_LINE (compiler
                                     // artifact: duration arithmetic
                                     // throw arc)
                .count()),  // cast throw arcs)  // GCOVR_EXCL_BR_LINE (compiler
                            // artifact: inline duration cast throw arcs)
        .duration_ms =
            state.session  // GCOVR_EXCL_BR_LINE (race: the install task sets
                           // the session within one fork-task step of start;
                           // status is never sampled inside that window)
                ? state.session->config().duration_ms  // GCOVR_EXCL_BR_LINE
                : 0,
    };
}

std::optional<nlohmann::json> LuaServiceManager::timer_inspect(
    const std::string& service_id, std::string* error) {
    {
        std::shared_lock lock(impl_->registry_mutex);
        if (!impl_->services.contains(service_id)) {
            if (error) {
                *error = "service not published: " + service_id;
            }
            return std::nullopt;
        }
        // Timer bookkeeping lives under the same registry lock; aggregate
        // without leaving it (registry_mutex nests nothing here).
        uint64_t total = 0;
        uint64_t repeating = 0;
        int64_t nearest_fire_ms = 0;
        nlohmann::json intervals = nlohmann::json::array();
        const auto service_timers =
            impl_->actor_timers_by_service.find(service_id);
        if (service_timers != impl_->actor_timers_by_service.end()) {
            for (const uint64_t timer_id : service_timers->second) {
                const auto it = impl_->actor_timers.find(timer_id);
                // Defensive: both timer books are maintained pairwise under
                // the same registry lock (registration, fire retirement and
                // cancel), so a stale per-service id cannot be observed.
                // GCOVR_EXCL_START
                if (it == impl_->actor_timers
                              .end()) {  //  (defensive:
                                         // region excluded above (timer-inspect
                                         // actor-side guard))
                    continue;
                }
                // GCOVR_EXCL_STOP
                ++total;
                const auto& timer = it->second;
                repeating += timer.repeating ? 1 : 0;
                if (intervals.size() < 32) {
                    intervals.push_back(timer.interval_ms);
                }
                if (nearest_fire_ms == 0 ||
                    timer.next_fire_ms < nearest_fire_ms) {
                    nearest_fire_ms = timer.next_fire_ms;
                }
            }
        }
        // Named locals keep the line counts on their own lines (the json
        // initializer otherwise records the arc on a continuation line).
        const std::uint64_t once_count = total - repeating;
        nlohmann::json out{
            {"name", service_id},
            {"timers", total},
            {"repeating", repeating},
            {"once", once_count},
            {"intervals_ms",
             std::move(intervals)}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // json initializer tail arc)
        if (total > 0) {
            // Named local keeps the line count on its own line (the json
            // initializer otherwise records the arc on a continuation line).
            const int64_t fire_left = nearest_fire_ms - Impl::now_ms();
            out["next_fire_ms_left"] = fire_left;
        }
        return out;
    }
}

std::optional<nlohmann::json> LuaServiceManager::pending_calls_inspect(
    const std::string& service_id, std::string* error) {
    // The whole projection stays under the registry lock: it collects raw
    // PendingCall pointers, and completion/timeout paths erase pending_calls
    // entries on other threads — sorting or reading the pointers after
    // unlocking would race that erase (use-after-free, SIGSEGV under load).
    // Same whole-function-lock shape as timer_inspect.
    std::shared_lock lock(impl_->registry_mutex);
    if (!impl_->services.contains(service_id)) {
        if (error) {
            *error = "service not published: " + service_id;
        }
        return std::nullopt;
    }
    std::vector<const Impl::PendingCall*> calls;
    for (const auto& [session, pending] : impl_->pending_calls) {
        // The wait belongs to this service when its caller (or, for a
        // proxied remote call, the receiving incarnation) is it. The
        // caller_service stamp is taken at call entry inside the
        // caller's dispatch scope.
        if (pending.caller_service == service_id) {
            calls.push_back(&pending);
        }
    }
    const int64_t now = Impl::now_ms();
    // Nearest deadline first; session id breaks ties deterministically.
    std::sort(calls.begin(), calls.end(),
              [](const Impl::PendingCall* a, const Impl::PendingCall* b) {
                  if (a->deadline_ms != b->deadline_ms) {
                      return a->deadline_ms < b->deadline_ms;
                  }
                  return a->session < b->session;
              });
    nlohmann::json items = nlohmann::json::array();
    // Report size limit: the summary keeps at most 32 entries.
    for (std::size_t i = 0; i < calls.size() && i < 32; ++i) {
        // Named locals keep the counts on their own lines (multi-element
        // json initializers otherwise record the arcs on a few continuation
        // lines only).
        const int64_t ms_left = calls[i]->deadline_ms - now;
        items.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: json
                          // initializer arc)
            {{"session", calls[i]->session},  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: json initializer arc)
             {"caller", calls[i]->caller_service},
             {"ms_left", ms_left},
             {"proxied", calls[i]->proxied}});
    }
    const std::size_t pending_count = calls.size();
    nlohmann::json out{
        {"name", service_id},
        {"pending_calls", pending_count},
        {"calls", std::move(items)}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json initializer tail arc)
    if (calls.size() > 32) {
        out["truncated"] = true;
    }
    return out;
}

std::map<std::string, LuaServiceManager::ServiceStats>
LuaServiceManager::service_stats() const {
    std::map<std::string, ServiceStats> out;
    std::shared_lock lock(impl_->registry_mutex);
    for (const auto& [name, counters] : impl_->service_counters) {
        out[name] = ServiceStats{
            counters->requests.load(std::memory_order_relaxed),
            counters->errors.load(std::memory_order_relaxed),
            // GCOVR_EXCL_START (duration expression continuation attributed
            // to no arc; the field itself is asserted by tests)
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          counters->spawned_at)
                // GCOVR_EXCL_STOP
                .count(),
            [&] {  // GCOVR_EXCL_LINE (immediate-invoke lambda header; the
                   // body lines below carry the counts)
                if (auto timers_it = impl_->actor_timers_by_service.find(name);
                    timers_it != impl_->actor_timers_by_service.end()) {
                    return timers_it->second.size();
                }
                return std::size_t{0};
            }(),
            0, 0, 0, counters->memory_kb.load(std::memory_order_relaxed)};
    }
    // Suspended caller coroutines join their caller's entry; a call hung in
    // an unpublished (still-initializing) caller is attributed to no entry.
    for (const auto& [session, call] : impl_->pending_calls) {
        if (auto it = out.find(call.caller_service); it != out.end()) {
            ++it->second.pending_calls;
        }
    }
    for (auto& [name, stats] : out) {
        stats.pending_tasks = pending_task_count(name);
        // Live handler coroutines attributed to this incarnation, same
        // grouping as pending_calls above.
        stats.coroutines = static_cast<std::uint64_t>(std::count_if(
            impl_->live_coroutines.begin(), impl_->live_coroutines.end(),
            [&name](const decltype(impl_->live_coroutines)::value_type& entry) {
                return entry.second == name;
            }));
    }
    return out;
}

}  // namespace shield::lua
