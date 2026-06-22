// [SHIELD_PLUGIN] PluginHost — the scan→catalog→plan→resolve→load→create→start
// pipeline orchestrator + host_api_v1 implementation.
#include "shield/plugin/plugin_host.hpp"

#include "schema_validator.hpp"
#include "shield/log/logger.hpp"

#include <lua.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace shield::plugin {

namespace fs = std::filesystem;

namespace {
const char* state_name(State s) {
    switch (s) {
        case State::planned:     return "planned";
        case State::loaded:      return "loaded";
        case State::started:     return "started";
        case State::unavailable: return "unavailable";
        case State::failed:      return "failed";
        case State::stopped:     return "stopped";
    }
    return "unknown";
}
}  // namespace

// CtxBundle and Impl are defined in plugin_host.hpp (they hold a unique_ptr
// and a vector of unique_ptrs, so they must be complete in the header).

PluginHost::PluginHost() : impl_(std::unique_ptr<Impl>(new Impl)) {}
PluginHost::~PluginHost() { shutdown(); }

// ---------------------------------------------------------------------------
// scan: read every <dir>/<pkg>/plugin.json, no code loaded.
// ---------------------------------------------------------------------------
void PluginHost::scan(const std::string& directory) {
    fs::path dir(directory);
    std::error_code ec;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_directory()) continue;
        fs::path json_path = entry.path() / "plugin.json";
        if (!fs::exists(json_path)) continue;
        try {
            Package pkg;
            pkg.manifest = load_manifest_file(json_path);
            pkg.root = entry.path();
            packages_.push_back(std::move(pkg));
        } catch (const std::exception& e) {
            SHIELD_LOG_WARNING(shield::log::get_logger("plugin"),
                               std::string("scan: skipping bad manifest ") +
                                   json_path.string() + ": " + e.what());
        }
    }
}

// ---------------------------------------------------------------------------
// catalog: validate package ids are unique + platform library path present.
// ---------------------------------------------------------------------------
bool PluginHost::catalog(std::string& error) {
    std::set<std::string> seen;
    for (const auto& p : packages_) {
        if (!seen.insert(p.manifest.id).second) {
            error = "plugin.manifest.invalid: duplicate package id '" +
                    p.manifest.id + "'";
            return false;
        }
        if (platform_library_path(p.manifest).empty()) {
            error = "plugin.manifest.invalid: package '" + p.manifest.id +
                    "' missing library path for current platform";
            return false;
        }
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
    for (const auto& d : cfg.instances) {
        const Package* pkg = find_package(d.package);
        if (!pkg) {
            error = "plugin.package.not_found: package '" + d.package +
                    "' for instance '" + d.id + "'";
            return false;
        }
        Instance inst;
        inst.id = d.id;
        inst.package = pkg;
        inst.decl = d;
        instances_.push_back(std::move(inst));
    }

    // resolve: each declared dependency must exist + provide the interface
    for (auto& inst : instances_) {
        for (const auto& req : inst.package->manifest.requires_) {
            auto it = inst.decl.dependencies.find(req.name);
            if (it == inst.decl.dependencies.end()) {
                if (req.optional) continue;
                error = "plugin.dependency.missing: instance '" + inst.id +
                        "' missing required dependency '" + req.name + "'";
                return false;
            }
            const Instance* dep = find_instance(it->second);
            if (!dep) {
                error = "plugin.dependency.missing: instance '" + inst.id +
                        "' dependency '" + req.name + "' -> '" + it->second +
                        "' not found";
                return false;
            }
            bool iface_ok = false;
            for (const auto& p : dep->package->manifest.provides) {
                if (p.interface == req.interface) { iface_ok = true; break; }
            }
            if (!iface_ok) {
                if (req.optional) continue;
                error = "plugin.dependency.missing: instance '" + dep->id +
                        "' does not provide '" + req.interface + "'";
                return false;
            }
            inst.dep_ids.push_back(dep->id);
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
        for (const auto& [k, v] : work) if (v == 0) q.push_back(k);
        size_t visited = 0;
        while (!q.empty()) {
            auto n = q.front(); q.pop_front(); ++visited;
            auto it = adj.find(n);
            if (it != adj.end())
                for (const auto& m : it->second)
                    if (--work[m] == 0) q.push_back(m);
        }
        if (visited != instances_.size()) {
            error = "plugin.dependency.cycle: circular dependency among instances";
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
        for (const auto& [k, v] : indeg) if (v == 0) q.push_back(k);
        auto remaining = indeg;
        while (!q.empty()) {
            auto n = q.front(); q.pop_front();
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
            inst.last_error = "plugin.entry.missing: " + libpath.string() + ": " + err;
            error = inst.last_error;
            return false;
        }
        using get_fn = const shield_plugin_abi_v1* (*)();
        auto get = reinterpret_cast<get_fn>(
            inst.lib.resolve(inst.package->manifest.entry.c_str()));
        if (!get) {
            inst.last_error = "plugin.entry.missing: symbol '" +
                              inst.package->manifest.entry + "' not found in " +
                              libpath.string();
            error = inst.last_error;
            return false;
        }
        inst.abi = get();
        if (!inst.abi || inst.abi->abi_version != SHIELD_PLUGIN_ABI_VERSION) {
            inst.last_error = "plugin.abi.mismatch: " + inst.id + " (abi_version)";
            error = inst.last_error;
            return false;
        }
        if (inst.abi->struct_size < sizeof(shield_plugin_abi_v1)) {
            inst.last_error = "plugin.abi.mismatch: " + inst.id + " (struct_size)";
            error = inst.last_error;
            return false;
        }
        if (!inst.abi->package_id ||
            std::string(inst.abi->package_id) != inst.package->manifest.id) {
            inst.last_error = "plugin.abi.mismatch: " + inst.id +
                              " (package_id '" +
                              (inst.abi->package_id ? inst.abi->package_id : "") +
                              "' != manifest '" + inst.package->manifest.id + "')";
            error = inst.last_error;
            return false;
        }
        inst.state = State::loaded;
    }
    return true;
}

// ---------------------------------------------------------------------------
// host_api_v1 table (stateless lambdas; per-instance state via CtxBundle*)
// ---------------------------------------------------------------------------
// Current Lua state for the thread doing inject/register_lua. Used by the
// lua_state host callback so plugins can fetch L from inside create/start
// (though typical use is to take L from the register_lua argument).
thread_local lua_State* g_current_lua_state = nullptr;

const shield_host_api_v1& PluginHost::host_api_table() {
    static shield_host_api_v1 api{};
    static bool inited = false;
    if (inited) return api;
    inited = true;

    api.log = [](shield_log_level lv, const char* pkg, const char* inst,
                 const char* msg) {
        auto& log = shield::log::get_logger(pkg ? pkg : "plugin");
        std::string m = (inst ? std::string("[") + inst + "] " : std::string()) +
                        (msg ? msg : "");
        switch (lv) {
            case SHIELD_LOG_DEBUG: SHIELD_LOG_DEBUG(log, m); break;
            case SHIELD_LOG_INFO:  SHIELD_LOG_INFO(log, m); break;
            case SHIELD_LOG_WARN:  SHIELD_LOG_WARNING(log, m); break;
            case SHIELD_LOG_ERROR: SHIELD_LOG_ERROR(log, m); break;
        }
    };
    api.report_error = [](const shield_error_v1* err) {
        if (!err) return;
        auto& log = shield::log::get_logger(err->package_id ? err->package_id : "plugin");
        SHIELD_LOG_ERROR(log, std::string("plugin error [") +
                                 (err->code ? err->code : "?") + "] " +
                                 (err->message ? err->message : "") +
                                 (err->instance_id ? " instance=" + std::string(err->instance_id) : "") +
                                 (err->phase ? " phase=" + std::string(err->phase) : ""));
    };
    api.config_get = [](shield_plugin_context_v1* ctx, const char* path) -> const char* {
        if (!ctx || !path) return nullptr;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->instance) return nullptr;
        // dot-path navigation into the instance's validated config
        const nlohmann::json* cur = &c->instance->decl.config;
        std::string p(path);
        size_t start = 0;
        while (start < p.size() && cur->is_object()) {
            size_t dot = p.find('.', start);
            std::string seg = (dot == std::string::npos) ? p.substr(start)
                                                         : p.substr(start, dot - start);
            if (!cur->contains(seg)) return nullptr;
            cur = &(*cur)[seg];
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        // Write into per-instance scratch (NOT thread_local): concurrent
        // config_get from two instances must not clobber each other's return.
        // Contract: valid until next config_get on THIS context.
        if (cur->is_string()) c->config_get_scratch = cur->get<std::string>();
        else c->config_get_scratch = cur->dump();
        return c->config_get_scratch.c_str();
    };
    api.dependency = [](shield_plugin_context_v1* ctx, const char* name,
                        const char* iface) -> const void* {
        if (!ctx || !name || !iface) return nullptr;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->host || !c->instance) return nullptr;
        auto it = c->instance->decl.dependencies.find(name);
        if (it == c->instance->decl.dependencies.end()) return nullptr;
        const Instance* dep = c->host->find_instance(it->second);
        if (!dep || !dep->handle || dep->state != State::started) return nullptr;
        shield_error_v1 e{};
        return dep->handle->get_interface(dep->handle, iface, &e);
    };
    api.lua_state = [](shield_plugin_context_v1*) -> lua_State* {
        // Return the Lua state currently being injected/registered on this
        // thread. Outside of inject_lua_paths/register_lua_all this is NULL.
        return g_current_lua_state;
    };
    api.lua_add_path = [](shield_plugin_context_v1* ctx, const char* path,
                          int is_cpath) -> int {
        if (!ctx || !path) return -1;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->instance || !c->instance->package) return -1;
        lua_State* L = g_current_lua_state;
        if (!L) return -1;

        // Resolve relative paths against the package root.
        fs::path resolved = c->instance->package->root;
        resolved /= path;
        // Convert "lua/?.lua" pattern-style paths to absolute.
        std::string s = resolved.string();
#ifdef _WIN32
        std::replace(s.begin(), s.end(), '\\', '/');
#endif
        lua_getglobal(L, "package");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
        const char* field = is_cpath ? "cpath" : "path";
        lua_getfield(L, -1, field);
        std::string cur = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!cur.empty() && cur.back() != ';') cur.push_back(';');
        cur += s;
        lua_pushlstring(L, cur.c_str(), cur.size());
        lua_setfield(L, -2, field);
        lua_pop(L, 1);
        return 0;
    };
    return api;
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

        nlohmann::json cfg = inst.decl.config;
        try { apply_defaults(inst.package->manifest.config_schema, cfg); } catch (...) {}
        std::string cfg_str = cfg.dump();

        shield_plugin_create_args_v1 args{};
        args.host_api = &host_api_table();
        args.ctx = reinterpret_cast<shield_plugin_context_v1*>(bundle.get());
        args.instance_id = inst.id.c_str();
        args.config_json = cfg_str.c_str();

        shield_error_v1 e{};
        if (!inst.abi->create ||
            inst.abi->create(&args, &inst.handle, &e) != 0 || !inst.handle) {
            inst.last_error = std::string("plugin.create.failed: ") + inst.id +
                              (e.code ? " [" + std::string(e.code) + "]" : "") +
                              (e.message ? " " + std::string(e.message) : "");
            error = inst.last_error;
            return false;
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
        if (!inst || !inst->handle) continue;
        shield_error_v1 e{};
        if (inst->handle->start && inst->handle->start(inst->handle, &e) != 0) {
            if (inst->decl.required) {
                inst->last_error = std::string("plugin.init.failed: ") + inst->id +
                                   (e.message ? " " + std::string(e.message) : "");
                inst->state = State::failed;
                error = inst->last_error;
                return false;
            }
            inst->last_error = "unavailable: " + inst->id;
            inst->state = State::unavailable;
        } else {
            inst->state = State::started;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lua integration: path injection + register_lua dispatch.
//
// Both run per Lua VM (services each get their own state). For each VM, the
// host sets g_current_lua_state so the host_api.lua_state / lua_add_path
// callbacks can reach the active state when a plugin queries them during
// register_lua.
// ---------------------------------------------------------------------------
void PluginHost::inject_lua_paths(lua_State* L) {
    if (!L) return;
    lua_State* prev = g_current_lua_state;
    g_current_lua_state = L;
    for (const auto& id : impl_->start_order) {
        const Instance* inst = find_instance(id);
        if (!inst || !inst->package || inst->state != State::started) continue;
        if (!inst->package->manifest.lua.enabled) continue;
        for (const auto& rel : inst->package->manifest.lua.search_paths) {
            if (rel.empty()) continue;
            // Resolve relative to the package root.
            fs::path resolved = inst->package->root / rel;
            std::string s = resolved.string();
#ifdef _WIN32
            std::replace(s.begin(), s.end(), '\\', '/');
#endif
            lua_getglobal(L, "package");
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
            lua_getfield(L, -1, "path");
            std::string cur = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            if (!cur.empty() && cur.back() != ';') cur.push_back(';');
            cur += s;
            lua_pushlstring(L, cur.c_str(), cur.size());
            lua_setfield(L, -2, "path");
            lua_pop(L, 1);
        }
    }
    g_current_lua_state = prev;
}

bool PluginHost::register_lua_all(lua_State* L, std::string& error) {
    if (!L) return true;
    lua_State* prev = g_current_lua_state;
    g_current_lua_state = L;
    bool ok = true;
    for (const auto& id : impl_->start_order) {
        Instance* inst = find_instance_mut(id);
        if (!inst || !inst->handle || inst->state != State::started) continue;
        // Transitional: plugins built before register_lua existed have a NULL
        // slot. Treat as "no Lua surface" and skip silently.
        if (!inst->handle->register_lua) continue;
        shield_error_v1 e{};
        if (inst->handle->register_lua(inst->handle, L, &e) != 0) {
            std::string msg = std::string("plugin.lua_register.failed: ") + inst->id +
                              (e.code ? " [" + std::string(e.code) + "]" : "") +
                              (e.message ? " " + std::string(e.message) : "");
            if (inst->decl.required) {
                inst->last_error = msg;
                inst->state = State::failed;
                error = msg;
                ok = false;
                break;
            }
            SHIELD_LOG_WARNING(shield::log::get_logger("plugin"),
                               "non-required register_lua failed: " + msg);
            inst->last_error = msg;
        }
    }
    g_current_lua_state = prev;
    return ok;
}

// ---------------------------------------------------------------------------
// startup: full pipeline.
// ---------------------------------------------------------------------------
bool PluginHost::startup(const PluginConfig& cfg, std::string& error) {
    scan(cfg.directory);
    if (!catalog(error)) return false;
    if (!plan_and_resolve(cfg, error)) return false;
    if (!load_all(error)) return false;
    if (!create_all(error)) return false;
    if (!start_all(error)) return false;
    for (const auto& i : instances_) {
        if (i.decl.required && i.state != State::started) {
            error = "plugin.init.failed: required instance '" + i.id +
                    "' not started (" + state_name(i.state) + ")";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// shutdown: reverse order, then clear all state.
// ---------------------------------------------------------------------------
void PluginHost::shutdown() {
    // Stop in reverse topological order (dependents first). We invoke each
    // instance's shutdown callback but DO NOT dlclose the libraries or clear
    // instances_ here: other host subsystems may still hold raw vtable
    // pointers resolved from these libraries, and
    // those must remain valid until process exit. Libraries are released when
    // the PluginHost itself is destroyed (process teardown — by then every
    // data pool that resolved a vtable has already been torn down, because
    // global_host() is constructed before database() in bootstrap).
    for (auto it = impl_->start_order.rbegin(); it != impl_->start_order.rend(); ++it) {
        Instance* inst = find_instance_mut(*it);
        if (!inst) continue;
        if (inst->handle && inst->state == State::started) {
            if (inst->handle->shutdown) inst->handle->shutdown(inst->handle);
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
            if (!inst || !inst->handle || inst->state != State::started) return nullptr;
            shield_error_v1 e{};
            return inst->handle->get_interface(inst->handle, interface_name, &e);
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
}

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
        for (const auto& pr : p.manifest.provides) info.provides.push_back(pr.interface);
        info.docs_url = p.manifest.documentation.url;
        info.docs_description = p.manifest.documentation.description;
        v.push_back(std::move(info));
    }
    return v;
}

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
}

std::optional<BindingInfo> PluginHost::get_binding(std::string_view name) const {
    for (const auto& b : impl_->bindings) {
        if (b.logical == name) {
            BindingInfo info;
            info.logical = b.logical;
            info.instance_id = b.instance_id;
            const Instance* inst = find_instance(b.instance_id);
            if (inst && !inst->package->manifest.provides.empty())
                info.interface = inst->package->manifest.provides.front().interface;
            return info;
        }
    }
    return std::nullopt;
}

PluginHost& global_host() {
    static PluginHost h;
    return h;
}

}  // namespace shield::plugin
