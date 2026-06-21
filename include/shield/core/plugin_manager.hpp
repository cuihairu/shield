// [SHIELD_CORE] Plugin manager for loading and managing plugins
#pragma once

#include "shield/plugin/plugin.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shield::core {

/// @brief Information about a loaded plugin
struct PluginInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string path;
    shield_plugin_type type;
    bool initialized = false;
};

/// @brief Plugin manager: discovers, loads, initializes, and manages plugins.
///
/// Usage:
///   PluginManager pm;
///   pm.discover("./plugins");
///   pm.init_all(config);
///   auto* db = pm.get_vtable<shield_db_plugin>("shield_db_mysql");
///   pm.shutdown_all();
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Non-copyable
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// @brief Discover plugins in a directory (scan for .dll/.so/.dylib)
    void discover(const std::string& plugin_dir);

    /// @brief Load a single plugin by path
    bool load(const std::string& path, std::string& error);

    /// @brief Load a plugin by name (searches in discovered paths)
    bool load_by_name(const std::string& name, std::string& error);

    /// @brief Initialize all loaded plugins
    /// @param config_map map of plugin_name -> config items
    /// @return true if all plugins initialized successfully
    bool init_all(const std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>& config_map,
                  std::string& error);

    /// @brief Shutdown all plugins (reverse order of init)
    void shutdown_all();

    /// @brief Find a plugin by name
    const shield_plugin* find(const std::string& name) const;

    /// @brief Get all plugins of a given type
    std::vector<const shield_plugin*> by_type(shield_plugin_type type) const;

    /// @brief List all loaded plugins
    std::vector<PluginInfo> list() const;

    /// @brief Check if a plugin is loaded
    bool is_loaded(const std::string& name) const;

    /// @brief Get the vtable for a plugin (type-safe cast)
    /// @tparam VtableType The expected vtable type (e.g. shield_db_plugin)
    /// @param plugin_name The plugin name
    /// @return Pointer to the vtable, or nullptr if not found/wrong type
    template<typename VtableType>
    const VtableType* get_vtable(const std::string& plugin_name) const {
        const shield_plugin* p = find(plugin_name);
        if (!p || !p->vtable) return nullptr;
        return static_cast<const VtableType*>(p->vtable);
    }

    /// @brief Get the number of loaded plugins
    size_t count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::core
