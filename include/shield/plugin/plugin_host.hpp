// [SHIELD_PLUGIN] Plugin system v1 — host (C++ side).
//
// C++ data model + PluginHost orchestrator for the scan → catalog → plan →
// resolve → load → create → start pipeline. Business code (e.g. shield::data
// DatabasePool) reaches providers via PluginHost::get_by_binding<T>().
//
// See docs/plugin-system.md for the authoritative design and
// docs/superpowers/specs/2026-06-22-plugin-system-v1-refactor-design.md for
// the refactor spec.
#pragma once

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_library.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shield::config { class Config; }

namespace shield::plugin {

// ---------------------------------------------------------------------------
// Declarative model (from JSON manifest + YAML config subtree)
// ---------------------------------------------------------------------------

// Parsed plugin.json. One per package directory.
struct Manifest {
    int schema_version = 1;
    std::string id;
    std::string name;
    std::string version;
    std::string kind;
    std::string description;
    std::string entry;  // C symbol, normally "shield_plugin_get_v1"

    struct Lib {
        std::string windows;
        std::string linux;
        std::string macos;
    } library;

    struct Provide {
        std::string interface;                  // e.g. "shield.database.v1"
        std::vector<std::string> capabilities;  // e.g. ["sql","transactions"]
    };
    std::vector<Provide> provides;

    struct Require {
        std::string name;        // local alias used in instance.dependencies
        std::string interface;   // required interface
        bool optional = false;
    };
    std::vector<Require> requires_;

    nlohmann::json config_schema;  // JSON-Schema subset for instance config
};

// One entry from plugins.instances[] (app.yaml plugins subtree).
struct InstanceDecl {
    std::string id;
    std::string package;
    bool required = true;
    std::map<std::string, std::string> dependencies;  // require name -> instance id
    nlohmann::json config;
};

// One entry from plugins.bindings{} (logical name -> instance id).
struct BindingDecl {
    std::string logical;       // e.g. "database.default"
    std::string instance_id;   // e.g. "db.main"
};

// Parsed plugins: subtree of app.yaml.
struct PluginConfig {
    std::string directory = "./plugins";
    std::vector<InstanceDecl> instances;
    std::vector<BindingDecl> bindings;
};

// ---------------------------------------------------------------------------
// Runtime model
// ---------------------------------------------------------------------------

// A catalogued package: manifest + on-disk root.
struct Package {
    Manifest manifest;
    std::filesystem::path root;
};

// Instance lifecycle state (matches docs/plugin-system.md state enum).
enum class State {
    planned,
    loaded,
    started,
    unavailable,
    failed,
    stopped
};

// A live instance under the host's management.
struct Instance {
    std::string id;
    const Package* package = nullptr;
    InstanceDecl decl;
    State state = State::planned;
    const shield_plugin_abi_v1* abi = nullptr;
    shield_plugin_instance_v1* handle = nullptr;
    PluginLibrary lib;
    std::vector<std::string> dep_ids;  // resolved dependency instance ids (topo)
    std::string last_error;            // structured error for failed/unavailable
};

// Introspection views (used by Lua + diagnostics).
struct PackageInfo {
    std::string id, version, kind;
    std::vector<std::string> provides;
};
struct InstanceInfo {
    std::string id, package, state;
    bool required = false;
};
struct BindingInfo {
    std::string logical, instance_id, interface;
};

// ---------------------------------------------------------------------------
// Manifest / config parsing (throw std::runtime_error on invalid input)
// ---------------------------------------------------------------------------

Manifest parse_manifest(const nlohmann::json& j);
Manifest load_manifest_file(const std::filesystem::path& plugin_json);

// Platform-specific relative library path declared in the manifest.
std::string platform_library_path(const Manifest& m);

PluginConfig parse_plugin_config(const shield::config::Config& cfg);
PluginConfig load_plugin_config();

// ---------------------------------------------------------------------------
// PluginHost
// ---------------------------------------------------------------------------

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Run the full pipeline. On failure returns false and sets `error` to a
    // structured message ("plugin.<phase>.<reason>: ...").
    bool startup(const PluginConfig& cfg, std::string& error);

    // Stop all started instances in reverse dependency order, then clear.
    void shutdown();

    // --- individual pipeline stages (exposed for testing) ---
    void scan(const std::string& directory);
    bool catalog(std::string& error);
    bool plan_and_resolve(const PluginConfig& cfg, std::string& error);
    bool load_all(std::string& error);
    bool create_all(std::string& error);
    bool start_all(std::string& error);

    // --- business access: binding name -> typed interface vtable ---
    // T must expose `static constexpr const char* interface_name`.
    template <typename Interface>
    const Interface* get_by_binding(std::string_view binding_name) const;

    // Look up the instance id behind a binding (empty if absent).
    std::string binding_instance_id(std::string_view binding_name) const;

    // --- introspection ---
    std::vector<std::string> package_ids() const;
    const Package* find_package(const std::string& id) const;
    const Instance* find_instance(const std::string& id) const;
    const std::vector<Instance>& instances() const { return instances_; }

    std::vector<PackageInfo> list_packages() const;
    std::vector<InstanceInfo> list_instances() const;
    std::optional<BindingInfo> get_binding(std::string_view name) const;

private:
    // Resolve a binding name to a raw interface vtable pointer (used by the
    // get_by_binding template). Returns nullptr if binding/instance/iface
    // is missing or the instance is not started.
    const void* get_binding_vtable(std::string_view binding,
                                   const char* interface_name) const;

    // Non-const lookup (used by load/create/start/shutdown to mutate state).
    Instance* find_instance_mut(const std::string& id);

    // Host-side context entity backing shield_plugin_context_v1*.
    struct CtxBundle {
        PluginHost* host;
        const Instance* instance;
        // Per-instance scratch buffer for config_get() return value. Documented
        // contract: "valid until the next config_get call on this context".
        // Using a per-instance member (NOT thread_local) avoids cross-instance
        // and cross-thread interference when multiple instances call config_get
        // concurrently.
        std::string config_get_scratch;
    };

    const shield_host_api_v1& host_api_table();

    struct Impl {
        std::vector<BindingDecl> bindings;
        // owned PluginHost-side contexts (one per created instance), stable
        // pointers handed to plugins as shield_plugin_context_v1*.
        std::vector<std::unique_ptr<CtxBundle>> contexts;
        std::vector<std::string> start_order;  // topo-ordered instance ids (deps first)
    };
    std::unique_ptr<Impl> impl_;

    std::vector<Package> packages_;
    std::vector<Instance> instances_;
};

// Process-wide host used by bootstrap + business code.
PluginHost& global_host();

// ---------------------------------------------------------------------------
// get_by_binding template (must be visible at point of instantiation)
// ---------------------------------------------------------------------------

template <typename Interface>
const Interface* PluginHost::get_by_binding(std::string_view binding_name) const {
    return static_cast<const Interface*>(
        get_binding_vtable(binding_name, Interface::interface_name));
}

}  // namespace shield::plugin
