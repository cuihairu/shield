#pragma once

#include <boost/dll/shared_library.hpp>
#include <boost/filesystem.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/core/starter.hpp"

namespace shield::core {

// Forward declaration
class ApplicationContext;

/**
 * @brief Plugin library handle wrapper using boost::dll
 */
using PluginLibrary = boost::dll::shared_library;

/**
 * @brief Plugin descriptor containing metadata about a loaded plugin
 */
struct PluginDescriptor {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::vector<std::string> dependencies;
    boost::filesystem::path library_path;
    PluginLibrary library;
    std::unique_ptr<IStarter> starter;

    PluginDescriptor() = default;
    PluginDescriptor(PluginDescriptor&&) = default;
    PluginDescriptor& operator=(PluginDescriptor&&) = default;

    // Disable copy operations due to unique_ptr
    PluginDescriptor(const PluginDescriptor&) = delete;
    PluginDescriptor& operator=(const PluginDescriptor&) = delete;
};

/**
 * @brief Plugin factory function signature
 */
using PluginCreateFunction = std::unique_ptr<IStarter> (*)();

/**
 * @brief Plugin info function signature
 */
struct PluginInfo {
    const char* name;
    const char* version;
    const char* description;
    const char* author;
    const char** dependencies;  // null-terminated array
};

using PluginInfoFunction = PluginInfo (*)();

/**
 * @brief Manages dynamic loading and lifecycle of plugins
 *
 * The PluginManager discovers, loads, and manages plugins (shared libraries)
 * that contain IStarter implementations. It provides plugin discovery from
 * directories, dependency resolution, and safe loading/unloading.
 */
class PluginManager {
public:
    /**
     * @brief Plugin loading event types
     */
    enum class PluginEvent {
        DISCOVERED,
        LOADING,
        LOADED,
        UNLOADING,
        UNLOADED,
        ERROR
    };

    /**
     * @brief Plugin event callback signature
     */
    using PluginEventCallback =
        std::function<void(PluginEvent event, const std::string& plugin_name,
                           const std::string& message)>;

public:
    PluginManager();
    ~PluginManager();

    /**
     * @brief Add a directory to search for plugins
     * @param directory_path Path to directory containing plugin libraries
     */
    void add_plugin_directory(const boost::filesystem::path& directory_path);

    /**
     * @brief Discover all plugins in registered directories
     * @return Number of plugins discovered
     */
    size_t discover_plugins();

    /**
     * @brief Load a specific plugin by name
     * @param plugin_name Name of the plugin to load
     * @return true if plugin loaded successfully
     */
    bool load_plugin(const std::string& plugin_name);

    /**
     * @brief Load all discovered plugins
     * @return Number of plugins successfully loaded
     */
    size_t load_all_plugins();

    /**
     * @brief Unload a specific plugin by name
     * @param plugin_name Name of the plugin to unload
     * @return true if plugin unloaded successfully
     */
    bool unload_plugin(const std::string& plugin_name);

    /**
     * @brief Unload all loaded plugins
     */
    void unload_all_plugins();

    /**
     * @brief Get a loaded plugin by name
     * @param plugin_name Name of the plugin
     * @return Pointer to plugin descriptor or nullptr if not found
     */
    const PluginDescriptor* get_plugin(const std::string& plugin_name) const;

    /**
     * @brief Get all loaded plugins
     * @return Vector of plugin names
     */
    std::vector<std::string> get_loaded_plugins() const;

    /**
     * @brief Get plugin count
     * @return Number of loaded plugins
     */
    size_t plugin_count() const { return loaded_plugins_.size(); }

    /**
     * @brief Check if a plugin is loaded
     * @param plugin_name Name of the plugin
     * @return true if plugin is loaded
     */
    bool is_plugin_loaded(const std::string& plugin_name) const;

    /**
     * @brief Set plugin event callback
     * @param callback Callback function for plugin events
     */
    void set_event_callback(PluginEventCallback callback);

    /**
     * @brief Get all starters from loaded plugins
     * @return Vector of starter pointers from plugins
     */
    std::vector<IStarter*> get_plugin_starters() const;

private:
    /**
     * @brief Load plugin from library file
     * @param library_path Path to the plugin library
     * @param plugin_name Name to assign to the plugin
     * @return true if loaded successfully
     */
    bool load_plugin_from_library(const boost::filesystem::path& library_path,
                                  const std::string& plugin_name);

    /**
     * @brief Validate plugin library has required symbols
     * @param library The loaded library
     * @return true if library is valid
     */
    bool validate_plugin_library(const PluginLibrary& library) const;

    /**
     * @brief Extract plugin info from library
     * @param library The loaded library
     * @return Plugin info structure
     */
    PluginInfo extract_plugin_info(const PluginLibrary& library) const;

    /**
     * @brief Emit plugin event
     * @param event Event type
     * @param plugin_name Plugin name
     * @param message Event message
     */
    void emit_event(PluginEvent event, const std::string& plugin_name,
                    const std::string& message = "");

    /**
     * @brief Get platform-specific library extension
     * @return Library file extension (.so, .dll, .dylib)
     */
    std::string get_library_extension() const;

    /**
     * @brief Resolve plugin loading order based on dependencies
     * @return Vector of plugin names in load order
     */
    std::vector<std::string> resolve_plugin_load_order() const;

private:
    std::vector<boost::filesystem::path> plugin_directories_;
    std::unordered_map<std::string, std::unique_ptr<PluginDescriptor>>
        loaded_plugins_;
    std::unordered_map<std::string, boost::filesystem::path>
        discovered_plugins_;
    PluginEventCallback event_callback_;
};

}  // namespace shield::core