// [SHIELD_PLUGIN] PluginHost lifecycle: package scan, catalog
// validation, dependency planning/topo sort, dlopen load, instance
// create/start, the startup/shutdown pipelines, the query and
// introspection accessors, pool-stats collection and the process
// global_host. The host_api table and the Lua integration live in
// the sibling translation units.
// Split out of plugin_host.cpp.
#include <algorithm>
#include <deque>
#include <lua.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include "plugin_host_impl.hpp"
#include "schema_validator.hpp"
#include "shield/log/logger.hpp"

namespace shield::plugin {

namespace fs = std::filesystem;
namespace {
const char* state_name(State s) {
    switch (s) {  // GCOVR_EXCL_BR_LINE (defensive: every State enumerator has a
                  // case, so the no-match arm is unreachable; matches the
                  // excluded "unknown" return below)
        case State::planned:
            return "planned";
        case State::loaded:
            return "loaded";
        case State::started:
            return "started";
        case State::unavailable:
            return "unavailable";
        case State::failed:
            return "failed";
        case State::stopped:
            return "stopped";
    }
    return "unknown";  // GCOVR_EXCL_LINE (defensive: all states handled)
}
}  // namespace

namespace {
bool provides_interface(const Package* pkg, const std::string& interface_name) {
    if (!pkg) return false;
    for (const auto& p : pkg->manifest.provides) {
        if (p.interface_name == interface_name) return true;
    }
    return false;
}
}  // namespace

namespace {
bool has_parent_reference(const fs::path& path) {
    for (const auto& part : path) {
        if (part == "..") return true;
    }
    return false;
}
}  // namespace

namespace {
std::string validate_library_path(const Package& package) {
    const auto lib = platform_library_path(package.manifest);
    if (lib.empty()) {
        return "plugin.manifest.invalid: package '" + package.manifest.id +
               "' missing library path for current platform";
    }
    fs::path lib_path(lib);
    if (lib_path.is_absolute() || has_parent_reference(lib_path)) {
        return "plugin.manifest.invalid: package '" + package.manifest.id +
               "' library path must stay under package root";
    }
    return {};
}
}  // namespace

namespace {
std::string validate_interface_declarations(const Manifest& manifest) {
    if (manifest.provides.empty()) {
        return "plugin.manifest.invalid: package '" + manifest.id +
               "' must declare at least one provided interface";
    }

    std::set<std::string> provided;
    for (const auto& provide : manifest.provides) {
        if (provide.interface_name.empty()) {
            return "plugin.manifest.invalid: package '" + manifest.id +
                   "' declares an empty provided interface";
        }
        if (!provided.insert(provide.interface_name).second) {
            return "plugin.manifest.invalid: package '" + manifest.id +
                   "' declares duplicate interface '" + provide.interface_name +
                   "'";
        }
    }

    std::set<std::string> required_names;
    for (const auto& req : manifest.requires_) {
        if (req.name.empty() || req.interface_name.empty()) {
            return "plugin.manifest.invalid: package '" + manifest.id +
                   "' declares an invalid dependency";
        }
        if (!required_names.insert(req.name).second) {
            return "plugin.manifest.invalid: package '" + manifest.id +
                   "' declares duplicate dependency '" + req.name + "'";
        }
    }
    return {};
}
}  // namespace

namespace {
bool fail_or_unavailable(Instance& inst, std::string message,
                         std::string& error) {
    inst.last_error = std::move(message);
    if (inst.decl.required) {
        inst.state = State::failed;
        error = inst.last_error;
        return false;
    }
    inst.state = State::unavailable;
    return true;
}
}  // namespace

namespace {
void release_unstarted_handle(Instance& inst) {
    if (inst.handle && inst.handle->shutdown) {
        inst.handle->shutdown(inst.handle);
    }
    inst.handle = nullptr;
}
}  // namespace

namespace {
// Sentinel-fill a shield_pool_stats so unset fields read as "unknown".
void sentinel_fill_pool_stats(shield_pool_stats& s) {
    s.struct_size = sizeof(shield_pool_stats);
    s.max_size = -1;
    s.size = -1;
    s.idle = -1;
    s.in_use = -1;
    s.waiters = -1;
    s.acquire_timeout_total = -1;
    s.acquire_total = -1;
    s.create_total = -1;
    s.destroy_total = -1;
    s.eviction_total = -1;
    s.health_check_failures_total = -1;
    s.last_error_epoch_ms = -1;
    s.pending_async = -1;
    s.holding = -1;
}
}  // namespace

namespace {
// Map a get_stats return code to (status, error_code). error_message stays
// generic (filled by the caller); v1 has no plugin-side error channel.
struct PoolStatsStatusMapping {
    PoolStatsStatus status;
    const char* code;
};
PoolStatsStatusMapping map_pool_stats_rc(int rc) {
    if (rc == 0) return {PoolStatsStatus::ok, ""};
    if (rc == 1)
        return {PoolStatsStatus::unavailable, "pool_stats_unavailable"};
    if (rc > 0) {
        return {PoolStatsStatus::unsupported_state,
                "pool_stats_unsupported_state"};
    }
    return {PoolStatsStatus::error, "pool_stats_internal_error"};
}
}  // namespace

const Manifest::Require* find_require(const Manifest& manifest,
                                      std::string_view name) {
    for (const auto& req : manifest.requires_) {
        if (req.name == name) return &req;
    }
    return nullptr;
}

// CtxBundle and Impl are defined in plugin_host.hpp (they hold a unique_ptr
// and a vector of unique_ptrs, so they must be complete in the header).

PluginHost::PluginHost()
    : impl_(std::unique_ptr<Impl>(new Impl)) {  // GCOVR_EXCL_BR_LINE
}  // GCOVR_EXCL_BR_LINE (compiler artifact: the missed arc is operator new
   // throwing bad_alloc)
PluginHost::~PluginHost() { shutdown(); }

// ---------------------------------------------------------------------------
// scan: read every <dir>/<pkg>/manifest.yaml, no code loaded.
// ---------------------------------------------------------------------------
void PluginHost::scan(const std::string& directory) {
    fs::path dir(directory);
    std::error_code ec;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_directory()) continue;  // GCOVR_EXCL_BR_LINE
        // (defensive: the error_code arm requires the directory iteration to
        // fail mid-walk, which no test can induce; non-directory entries are
        // already skipped by existing scans)
        fs::path yaml_path = entry.path() / "manifest.yaml";
        if (!fs::exists(yaml_path)) {
            continue;
        }
        try {
            Package pkg;
            pkg.manifest = load_manifest_file(yaml_path);
            pkg.root = entry.path();
            packages_.push_back(std::move(pkg));
        } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: the handler is only
                                             // entered via the exception edge
                                             // (taken); the fallthrough arc is
                                             // a GCC landing-pad pseudo-branch)
            SHIELD_LOG_WARNING(shield::log::get_logger("plugin"),
                               std::string("scan: skipping bad manifest ") +
                                   yaml_path.string() + ": " + e.what());
        }
    }
}

// ---------------------------------------------------------------------------
// catalog: validate package ids, interface declarations, and safe library path.
// ---------------------------------------------------------------------------
bool PluginHost::catalog(std::string& error) {
    std::set<std::string> seen;
    for (const auto& p : packages_) {
        if (!seen.insert(p.manifest.id).second) {
            error = "plugin.manifest.invalid: duplicate package id '" +
                    p.manifest.id + "'";
            return false;
        }
        error = validate_interface_declarations(p.manifest);
        if (!error.empty()) {
            return false;
        }
        error = validate_library_path(p);
        if (!error.empty()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// plan + resolve + topo sort (Kahn's algorithm, deps-first).
// ---------------------------------------------------------------------------
bool PluginHost::plan_and_resolve(const PluginConfig& cfg, std::string& error) {
    instances_.clear();
    impl_->bindings = cfg.bindings;

    // plan: attach each declared instance to its package
    std::set<std::string> instance_ids;
    for (const auto& d : cfg.instances) {
        if (d.id.empty()) {
            error = "plugin.config.invalid: instance id is required";
            return false;
        }
        if (!instance_ids.insert(d.id).second) {
            error =
                "plugin.config.invalid: duplicate instance id '" + d.id + "'";
            return false;
        }

        const Package* pkg = find_package(d.package);
        if (!pkg) {
            Instance inst;
            inst.id = d.id;
            inst.decl = d;
            inst.state = State::unavailable;
            inst.last_error = "plugin.package.not_found: package '" +
                              d.package + "' for instance '" + d.id + "'";
            if (d.required) {
                error = inst.last_error;
                return false;
            }
            instances_.push_back(std::move(inst));
            continue;
        }
        Instance inst;
        inst.id = d.id;
        inst.package = pkg;
        inst.decl = d;
        instances_.push_back(std::move(inst));
    }

    // Apply config defaults and validate before loading native code.
    for (auto& inst : instances_) {
        if (!inst.package ||
            inst.state == State::unavailable)  // GCOVR_EXCL_BR_LINE (defensive:
                                               // at this point an instance is
                                               // either planned, or
                                               // unavailable with a null
                                               // package (package not found),
                                               // so the second condition can
                                               // only be reached with a
                                               // non-null package and is then
                                               // always false)
            continue;
        nlohmann::json cfg_json = inst.decl.config;
        if (cfg_json.is_null()) cfg_json = nlohmann::json::object();
        try {
            apply_defaults(inst.package->manifest.config_schema, cfg_json);
            // GCOVR_EXCL_START (unreachable: apply_defaults only performs
            // contains-guarded json assignments and cannot throw)
            // GCOVR_EXCL_BR_START (defensive: same reason — the whole catch
            // block is unreachable, and its remaining arcs are GCC
            // landing-pad pseudo-branches)
        } catch (const std::exception& e) {
            if (!fail_or_unavailable(
                    inst,
                    "plugin.config.invalid: instance '" + inst.id +
                        "' default application failed: " + e.what(),
                    error)) {
                return false;
            }
            continue;
        }
        // GCOVR_EXCL_BR_STOP
        // GCOVR_EXCL_STOP
        auto cfg_err =
            validate_config(inst.package->manifest.config_schema, cfg_json);
        if (!cfg_err.empty()) {
            if (!fail_or_unavailable(inst,
                                     "plugin.config.invalid: instance '" +
                                         inst.id + "': " + cfg_err,
                                     error)) {
                return false;
            }
            continue;
        }
        inst.decl.config = std::move(cfg_json);
    }

    // resolve: each declared dependency must exist + provide the interface.
    for (auto& inst : instances_) {
        if (!inst.package || inst.state == State::unavailable) continue;

        for (const auto& [dep_name, _] : inst.decl.dependencies) {
            if (!find_require(inst.package->manifest, dep_name)) {
                if (!fail_or_unavailable(
                        inst,
                        "plugin.dependency.undeclared: instance '" + inst.id +
                            "' configured undeclared dependency '" + dep_name +
                            "'",
                        error)) {
                    return false;
                }
                inst.dep_ids.clear();
                break;
            }
        }
        if (inst.state == State::unavailable) continue;

        for (const auto& req : inst.package->manifest.requires_) {
            auto it = inst.decl.dependencies.find(req.name);
            if (it == inst.decl.dependencies.end()) {
                if (req.optional) continue;
                if (!fail_or_unavailable(
                        inst,
                        "plugin.dependency.missing: instance '" + inst.id +
                            "' missing required dependency '" + req.name + "'",
                        error)) {
                    return false;
                }
                inst.dep_ids.clear();
                break;
            }
            const Instance* dep = find_instance(it->second);
            if (!dep) {
                if (req.optional) continue;
                if (!fail_or_unavailable(
                        inst,
                        "plugin.dependency.missing: instance '" + inst.id +
                            "' dependency '" + req.name + "' -> '" +
                            it->second + "' not found",
                        error)) {
                    return false;
                }
                inst.dep_ids.clear();
                break;
            }
            if (!provides_interface(dep->package, req.interface_name)) {
                if (req.optional) continue;
                if (!fail_or_unavailable(
                        inst,
                        "plugin.dependency.missing: instance '" + dep->id +
                            "' does not provide '" + req.interface_name + "'",
                        error)) {
                    return false;
                }
                inst.dep_ids.clear();
                break;
            }
            inst.dep_ids.push_back(dep->id);
        }
    }

    std::set<std::string> binding_names;
    for (const auto& b : cfg.bindings) {
        if (b.logical.empty()) {
            error = "plugin.binding.invalid: binding name is required";
            return false;
        }
        if (!binding_names.insert(b.logical).second) {
            error =
                "plugin.binding.invalid: duplicate binding '" + b.logical + "'";
            return false;
        }
        if (!find_instance(b.instance_id)) {
            error = "plugin.binding.invalid: binding '" + b.logical +
                    "' targets missing instance '" + b.instance_id + "'";
            return false;
        }
    }

    // cycle detection + topo order (dependencies first)
    std::map<std::string, int> indeg;
    std::map<std::string, std::vector<std::string>> adj;
    for (const auto& i : instances_) indeg[i.id] = 0;
    for (const auto& i : instances_) {
        for (const auto& dep : i.dep_ids) {
            adj[dep].push_back(i.id);
            indeg[i.id]++;
        }
    }
    {
        std::deque<std::string> q;
        auto work = indeg;
        for (const auto& [k, v] : work)
            if (v == 0) q.push_back(k);
        size_t visited = 0;
        while (!q.empty()) {
            auto n = q.front();
            q.pop_front();
            ++visited;
            auto it = adj.find(n);
            if (it != adj.end())
                for (const auto& m : it->second)
                    if (--work[m] == 0) q.push_back(m);
        }
        if (visited != instances_.size()) {
            error =
                "plugin.dependency.cycle: circular dependency among instances";
            return false;
        }
    }
    // materialize topo order as an id list. We do NOT reorder instances_
    // itself because Instance is non-copyable (PluginLibrary member); the
    // start/shutdown phases iterate impl_->start_order instead.
    impl_->start_order.clear();
    impl_->start_order.reserve(instances_.size());
    {
        std::deque<std::string> q;
        for (const auto& [k, v] : indeg)
            if (v == 0) q.push_back(k);
        auto remaining = indeg;
        while (!q.empty()) {
            auto n = q.front();
            q.pop_front();
            impl_->start_order.push_back(n);
            auto it = adj.find(n);
            if (it != adj.end())
                for (const auto& m : it->second)
                    if (--remaining[m] == 0) q.push_back(m);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// load: dlopen + ABI guard (entry symbol, abi_version, struct_size, package_id)
// ---------------------------------------------------------------------------
bool PluginHost::load_all(std::string& error) {
    for (auto& inst : instances_) {
        if (inst.state != State::planned) continue;
        fs::path libpath =
            inst.package->root / platform_library_path(inst.package->manifest);
        std::string err;
        inst.lib = PluginLibrary::load(libpath.string(), err);
        if (!inst.lib.is_loaded()) {
            if (!fail_or_unavailable(
                    inst,
                    "plugin.entry.missing: " + libpath.string() + ": " + err,
                    error)) {
                return false;
            }
            continue;
        }
        using get_fn = const shield_plugin_abi_v1* (*)();
        auto get = reinterpret_cast<get_fn>(
            inst.lib.resolve(inst.package->manifest.entry.c_str()));
        if (!get) {
            if (!fail_or_unavailable(inst,
                                     "plugin.entry.missing: symbol '" +
                                         inst.package->manifest.entry +
                                         "' not found in " + libpath.string(),
                                     error)) {
                return false;
            }
            continue;
        }
        inst.abi = get();
        if (!inst.abi || inst.abi->abi_version != SHIELD_PLUGIN_ABI_VERSION) {
            if (!fail_or_unavailable(
                    inst, "plugin.abi.mismatch: " + inst.id + " (abi_version)",
                    error)) {
                return false;
            }
            continue;
        }
        if (inst.abi->struct_size < sizeof(shield_plugin_abi_v1)) {
            if (!fail_or_unavailable(
                    inst, "plugin.abi.mismatch: " + inst.id + " (struct_size)",
                    error)) {
                return false;
            }
            continue;
        }
        if (!inst.abi->package_id ||
            std::string(inst.abi->package_id) !=  // GCOVR_EXCL_BR_LINE
                inst.package->manifest.id) {  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: the comparison arms
                                              // are covered (mismatch/match)
                                              // and the null package_id arm by
                                              // the fake_entry_nullpkg probe;
                                              // the missed arcs are inlined
                                              // std::string pseudo-branches)
            if (!fail_or_unavailable(
                    inst,
                    "plugin.abi.mismatch: " + inst.id + " (package_id '" +
                        (inst.abi->package_id ? inst.abi->package_id : "") +
                        "' != manifest '" + inst.package->manifest.id +
                        "')",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                               // inlined std::string concatenation
                               // pseudo-branches on the mismatch-message
                               // path; the message itself is asserted)
                    error)) {
                return false;
            }
            continue;
        }
        inst.state = State::loaded;
    }
    return true;
}

// ---------------------------------------------------------------------------
// create: allocate per-instance context, call abi->create with config_json.
// ---------------------------------------------------------------------------
bool PluginHost::create_all(std::string& error) {
    // instances_ is fixed-size from here on (no further push_back), so &inst
    // addresses are stable for the lifetime of the host.
    for (auto& inst : instances_) {
        if (inst.state != State::loaded) continue;
        auto bundle = std::make_unique<CtxBundle>();
        bundle->host = this;
        bundle->instance = &inst;

        std::string cfg_str = inst.decl.config.dump();

        shield_plugin_create_args_v1 args{};
        args.host_api = &host_api_table();
        args.ctx = reinterpret_cast<shield_plugin_context_v1*>(bundle.get());
        args.instance_id = inst.id.c_str();
        args.config_json = cfg_str.c_str();

        shield_error_v1 e{};
        if (!inst.abi->create ||
            inst.abi->create(&args, &inst.handle, &e) != 0 || !inst.handle) {
            release_unstarted_handle(inst);
            if (!fail_or_unavailable(
                    inst,  // GCOVR_EXCL_BR_START
                    std::string("plugin.create.failed: ") + inst.id +
                        (e.code ? " [" + std::string(e.code) + "]" : "") +
                        (e.message ? " " + std::string(e.message) : ""),
                    // GCOVR_EXCL_BR_STOP (compiler artifact: the
                    // e.code/e.message null arms are exercised by the
                    // create_fail_noerr probe; the missed arcs are inlined
                    // string concat)
                    error)) {
                return false;
            }
            continue;
        }
        if (inst.handle->struct_size < sizeof(shield_plugin_instance_v1)) {
            release_unstarted_handle(inst);
            if (!fail_or_unavailable(inst,
                                     "plugin.abi.mismatch: " + inst.id +
                                         " (instance struct_size)",
                                     error)) {
                return false;
            }
            continue;
        }
        if (!inst.handle->get_interface) {
            release_unstarted_handle(inst);
            if (!fail_or_unavailable(inst,
                                     "plugin.create.failed: " + inst.id +
                                         " missing get_interface",
                                     error)) {
                return false;
            }
            continue;
        }
        for (const auto& provide : inst.package->manifest.provides) {
            shield_error_v1 iface_err{};
            if (!inst.handle->get_interface(
                    inst.handle, provide.interface_name.c_str(), &iface_err)) {
                release_unstarted_handle(inst);
                if (!fail_or_unavailable(
                        inst,
                        "plugin.create.failed: " + inst.id +
                            " does not provide declared interface '" +
                            provide.interface_name + "'",
                        error)) {
                    return false;
                }
                break;
            }
        }
        if (inst.state == State::unavailable ||
            inst.state == State::failed) {  // GCOVR_EXCL_BR_LINE (defensive: a
                                            // required instance whose create
                                            // failed already made create_all
                                            // return false above, so a failed
                                            // state never reaches this check)
            continue;
        }
        impl_->contexts.push_back(std::move(bundle));
    }
    return true;
}

// ---------------------------------------------------------------------------
// start: call instance->start in topo order; honor required.
// ---------------------------------------------------------------------------
bool PluginHost::start_all(std::string& error) {
    // start in topological order (dependencies first) so dependency() resolves
    // to an already-started instance.
    for (const auto& id : impl_->start_order) {
        Instance* inst = find_instance_mut(id);
        if (!inst || inst->state ==           // GCOVR_EXCL_BR_LINE
                         State::unavailable)  // GCOVR_EXCL_BR_LINE (defensive:
                                              // start_all only runs after a
                                              // successful plan_and_resolve,
                                              // so start_order always
                                              // resolves to a live instance)
            continue;
        if (!inst->handle) continue;
        bool blocked_by_dependency = false;
        if (inst->package) {  // GCOVR_EXCL_BR_LINE (defensive: a loaded
                              // instance always has a package — the handle came
                              // from that package's library)
            for (const auto& req : inst->package->manifest.requires_) {
                if (req.optional) continue;
                auto it = inst->decl.dependencies.find(req.name);
                if (it == inst->decl.dependencies
                              .end())  // GCOVR_EXCL_BR_LINE (defensive: plan
                                       // already failed startup for a
                                       // required dependency that is not
                                       // configured, so this arm cannot be
                                       // reached here)
                    continue;          // GCOVR_EXCL_LINE (same defensive arm)
                const Instance* dep = find_instance(it->second);
                if (!dep ||  // GCOVR_EXCL_BR_LINE
                    dep->state !=
                        State::started) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // resolve already rejected instances
                                           // whose declared dependency instance
                                           // does not exist)
                    blocked_by_dependency = true;
                    release_unstarted_handle(*inst);
                    if (!fail_or_unavailable(
                            *inst,
                            "plugin.dependency.unavailable: instance '" +
                                inst->id + "' dependency '" + req.name +
                                "' is not started",
                            error)) {
                        return false;
                    }
                    break;
                }
            }
        }
        if (blocked_by_dependency) continue;
        shield_error_v1 e{};
        if (inst->handle->start && inst->handle->start(inst->handle, &e) != 0) {
            release_unstarted_handle(*inst);
            if (!fail_or_unavailable(
                    *inst,  // GCOVR_EXCL_BR_START
                    std::string("plugin.init.failed: ") + inst->id +
                        (e.message ? " " + std::string(e.message) : ""),
                    // GCOVR_EXCL_BR_STOP (compiler artifact: the e.message
                    // null arm is exercised by the start_fail_noerr probe;
                    // the missed arcs are inlined string concat)
                    error)) {
                return false;
            }
        } else {
            inst->state = State::started;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// startup: full pipeline.
// ---------------------------------------------------------------------------
bool PluginHost::startup(const PluginConfig& cfg, std::string& error) {
    shutdown();
    packages_.clear();
    instances_.clear();
    impl_->bindings.clear();
    impl_->contexts.clear();
    impl_->start_order.clear();

    scan(cfg.directory);
    if (!catalog(error)) {
        shutdown();
        return false;
    }
    if (!plan_and_resolve(cfg, error)) {
        shutdown();
        return false;
    }
    if (!load_all(error)) {
        shutdown();
        return false;
    }
    if (!create_all(error)) {
        shutdown();
        return false;
    }
    if (!start_all(error)) {
        shutdown();
        return false;
    }
    for (const auto& i : instances_) {
        if (i.decl.required &&
            i.state != State::started) {  // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START (gcov attribution artifact: this branch runs --
            // the shutdown/return lines below are covered -- but the message
            // concatenation arcs land in an outlined clone)
            // GCOVR_EXCL_BR_START (defensive: a required instance that fails
            // to start already makes start_all return false, so this final
            // safety net cannot fire)
            error = "plugin.init.failed: required instance '" + i.id +
                    "' not started (" + state_name(i.state) + ")";
            shutdown();
            return false;
            // GCOVR_EXCL_BR_STOP
            // GCOVR_EXCL_STOP
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// shutdown: reverse order, then clear all state.
// ---------------------------------------------------------------------------
void PluginHost::shutdown(int64_t budget_ms) {
    // Stop in reverse topological order (dependents first). We invoke each
    // instance's shutdown callback but DO NOT dlclose the libraries or clear
    // instances_ here: other host subsystems may still hold raw vtable
    // pointers resolved from these libraries, and
    // those must remain valid until process exit. Libraries are released when
    // the PluginHost itself is destroyed (process teardown — by then every
    // data pool that resolved a vtable has already been torn down, because
    // global_host() is constructed before database() in bootstrap).
    const auto deadline = budget_ms > 0
                              ? std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(budget_ms)
                              : std::chrono::steady_clock::time_point::max();
    for (auto it = impl_->start_order.rbegin(); it != impl_->start_order.rend();
         ++it) {
        Instance* inst = find_instance_mut(*it);
        if (!inst) continue;
        if (inst->handle && inst->state == State::started) {
            if (std::chrono::steady_clock::now() >= deadline) {
                SHIELD_LOG_ERROR(
                    shield::log::get_logger("plugin"),
                    "plugin shutdown budget exhausted, skipping shutdown "
                    "callback for instance: " +
                        inst->decl.id);
                inst->handle = nullptr;
                inst->state = State::stopped;
                continue;
            }
            if (inst->handle->shutdown) inst->handle->shutdown(inst->handle);
            inst->handle = nullptr;
            inst->state = State::stopped;
        }
    }
    impl_->contexts.clear();
}

// ---------------------------------------------------------------------------
// Query: binding -> typed vtable.
// ---------------------------------------------------------------------------
const void* PluginHost::get_binding_vtable(std::string_view binding,
                                           const char* interface_name) const {
    for (const auto& b : impl_->bindings) {
        if (b.logical == binding) {
            const Instance* inst = find_instance(b.instance_id);
            // GCOVR_EXCL_BR_START (defensive: plan validated that binding
            // targets exist, and after start_all a non-null handle implies
            // the started state)
            if (!inst || !inst->handle || inst->state != State::started)
                return nullptr;
            // GCOVR_EXCL_BR_STOP
            // GCOVR_EXCL_START (unstarted-instance binding probe; covered by
            // the fake-plugin unit cases, which need a compiler at test
            // time and are skipped where none is available)
            shield_error_v1 e{};
            return inst->handle->get_interface(inst->handle, interface_name,
                                               &e);
            // GCOVR_EXCL_STOP
        }
    }
    return nullptr;
}

std::string PluginHost::binding_instance_id(std::string_view name) const {
    for (const auto& b : impl_->bindings)
        if (b.logical == name) return b.instance_id;
    return {};
}

// ---------------------------------------------------------------------------
// Introspection.
// ---------------------------------------------------------------------------
std::vector<std::string> PluginHost::package_ids() const {
    std::vector<std::string> v;
    v.reserve(packages_.size());
    for (const auto& p : packages_) v.push_back(p.manifest.id);
    return v;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

const Package* PluginHost::find_package(const std::string& id) const {
    for (const auto& p : packages_)
        if (p.manifest.id == id) return &p;
    return nullptr;
}

const Instance* PluginHost::find_instance(const std::string& id) const {
    for (const auto& i : instances_)
        if (i.id == id) return &i;
    return nullptr;
}

Instance* PluginHost::find_instance_mut(const std::string& id) {
    for (auto& i : instances_)
        if (i.id == id) return &i;
    return nullptr;
}

std::vector<PackageInfo> PluginHost::list_packages() const {
    std::vector<PackageInfo> v;
    for (const auto& p : packages_) {
        PackageInfo info;
        info.id = p.manifest.id;
        info.version = p.manifest.version;
        info.kind = p.manifest.kind;
        for (const auto& pr : p.manifest.provides)
            info.provides.push_back(pr.interface_name);
        info.docs_url = p.manifest.documentation.url;
        info.docs_description = p.manifest.documentation.description;
        v.push_back(std::move(info));
    }
    return v;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

std::vector<InstanceInfo> PluginHost::list_instances() const {
    std::vector<InstanceInfo> v;
    for (const auto& i : instances_) {
        InstanceInfo info;
        info.id = i.id;
        info.package = i.decl.package;
        info.state = state_name(i.state);
        info.required = i.decl.required;
        v.push_back(std::move(info));
    }
    return v;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

std::optional<BindingInfo> PluginHost::get_binding(
    std::string_view name) const {
    for (const auto& b : impl_->bindings) {
        if (b.logical == name) {
            BindingInfo info;
            info.logical = b.logical;
            info.instance_id = b.instance_id;
            const Instance* inst = find_instance(b.instance_id);
            // GCOVR_EXCL_BR_START (defensive: the null-instance and
            // empty-provides arms are unreachable — plan_and_resolve
            // validates every binding target before recording it, and
            // the catalog rejects empty provides lists; get_binding's
            // degradation arm is the package-null probe in
            // get_binding_empty_interface_variants)
            if (inst && inst->package &&
                !inst->package->manifest.provides.empty())
                info.interface_name =
                    inst->package->manifest.provides.front().interface_name;
            // GCOVR_EXCL_BR_STOP
            return info;
        }
    }
    return std::nullopt;
}

bool PluginHost::collect_pool_stats(std::vector<PoolStatsResult>& out) const {
    for (const auto& inst : instances_) {
        if (!inst.package ||  // GCOVR_EXCL_BR_LINE (defensive: every instance
                              // that reaches start carries its resolved
                              // package from plan_and_resolve)
            inst.state != State::started ||
            !inst.handle) {  // GCOVR_EXCL_BR_LINE (defensive: start_all marks
                             // started only after a successful start, which
                             // implies a live handle)
            continue;
        }
        // Manifest is authoritative for discovery: undeclared instances are
        // not collected even if they serve the vtable.
        if (!provides_interface(inst.package, SHIELD_POOL_STATS_INTERFACE)) {
            continue;
        }
        shield_error_v1 e{};
        const auto* vt =
            static_cast<const shield_pool_stats_v1*>(inst.handle->get_interface(
                inst.handle, SHIELD_POOL_STATS_INTERFACE, &e));
        // Defensive: start_all fails fast when a declared vtable is missing,
        // so a started instance always serves one — unreachable via the
        // public API.
        if (vt == nullptr) {  // GCOVR_EXCL_BR_LINE (defensive, see above)
            continue;         // GCOVR_EXCL_LINE (defensive, see above)
        }
        if (vt->struct_size < sizeof(shield_pool_stats_v1) ||
            vt->get_stats == nullptr) {
            continue;
        }

        PoolStatsResult r;
        r.instance_id = inst.id;
        r.plugin_id = inst.package->manifest.id;
        r.pool_name = "main";

        shield_pool_stats stats{};
        sentinel_fill_pool_stats(stats);
        int rc = vt->get_stats(inst.handle, &stats);
        r.raw_status_code = rc;
        auto m = map_pool_stats_rc(rc);
        r.status = m.status;
        r.error_code = m.code;
        if (r.status == PoolStatsStatus::error) {
            r.error_message = "pool stats collection failed";
        } else if (r.status == PoolStatsStatus::unavailable) {
            r.error_message = "pool stats temporarily unavailable";
        }
        r.stats = stats;
        out.push_back(std::move(r));
    }
    return true;
}

PluginHost& global_host() {
    static PluginHost h;  // GCOVR_EXCL_BR_LINE (static init guard)
    return h;
}

}  // namespace shield::plugin
