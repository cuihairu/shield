#include "shield/core/plugin_manager.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

#include "shield/log/logger.hpp"

namespace shield::core {

PluginManager::PluginManager() { SHIELD_LOG_DEBUG << "PluginManager created"; }

PluginManager::~PluginManager() {
    unload_all_plugins();
    SHIELD_LOG_DEBUG << "PluginManager destroyed";
}

void PluginManager::add_plugin_directory(
    const boost::filesystem::path& directory_path) {
    if (!boost::filesystem::exists(directory_path)) {
        SHIELD_LOG_WARN << "Plugin directory does not exist: "
                        << directory_path;
        return;
    }

    if (!boost::filesystem::is_directory(directory_path)) {
        SHIELD_LOG_WARN << "Path is not a directory: " << directory_path;
        return;
    }

    plugin_directories_.push_back(directory_path);
    SHIELD_LOG_INFO << "Added plugin directory: " << directory_path;
}

size_t PluginManager::discover_plugins() {
    discovered_plugins_.clear();
    std::string extension = get_library_extension();

    for (const auto& directory : plugin_directories_) {
        try {
            for (const auto& entry :
                 boost::filesystem::directory_iterator(directory)) {
                if (boost::filesystem::is_regular_file(entry.path()) &&
                    entry.path().extension() == extension) {
                    std::string plugin_name = entry.path().stem().string();
                    // Remove 'lib' prefix if present on Unix systems
                    if (plugin_name.substr(0, 3) == "lib") {
                        plugin_name = plugin_name.substr(3);
                    }

                    discovered_plugins_[plugin_name] = entry.path();
                    emit_event(PluginEvent::DISCOVERED, plugin_name,
                               "Found at: " + entry.path().string());

                    SHIELD_LOG_DEBUG << "Discovered plugin: " << plugin_name
                                     << " at " << entry.path();
                }
            }
        } catch (const boost::filesystem::filesystem_error& e) {
            SHIELD_LOG_ERROR << "Error scanning plugin directory " << directory
                             << ": " << e.what();
        }
    }

    SHIELD_LOG_INFO << "Discovered " << discovered_plugins_.size()
                    << " plugins";
    return discovered_plugins_.size();
}

bool PluginManager::load_plugin(const std::string& plugin_name) {
    // Check if already loaded
    if (is_plugin_loaded(plugin_name)) {
        SHIELD_LOG_WARN << "Plugin already loaded: " << plugin_name;
        return true;
    }

    // Find plugin in discovered plugins
    auto it = discovered_plugins_.find(plugin_name);
    if (it == discovered_plugins_.end()) {
        emit_event(PluginEvent::ERROR, plugin_name, "Plugin not discovered");
        SHIELD_LOG_ERROR << "Plugin not found in discovered plugins: "
                         << plugin_name;
        return false;
    }

    return load_plugin_from_library(it->second, plugin_name);
}

size_t PluginManager::load_all_plugins() {
    // First discover plugins if not done already
    if (discovered_plugins_.empty()) {
        discover_plugins();
    }

    // Resolve loading order based on dependencies
    auto load_order = resolve_plugin_load_order();

    size_t loaded_count = 0;
    for (const auto& plugin_name : load_order) {
        if (load_plugin(plugin_name)) {
            loaded_count++;
        }
    }

    SHIELD_LOG_INFO << "Loaded " << loaded_count << " out of "
                    << discovered_plugins_.size() << " plugins";
    return loaded_count;
}

bool PluginManager::unload_plugin(const std::string& plugin_name) {
    auto it = loaded_plugins_.find(plugin_name);
    if (it == loaded_plugins_.end()) {
        SHIELD_LOG_WARN << "Plugin not loaded: " << plugin_name;
        return false;
    }

    emit_event(PluginEvent::UNLOADING, plugin_name);

    try {
        // Reset starter first
        it->second->starter.reset();

        // Unload library - this will happen automatically when PluginLibrary
        // destructor is called
        loaded_plugins_.erase(it);

        emit_event(PluginEvent::UNLOADED, plugin_name);
        SHIELD_LOG_INFO << "Successfully unloaded plugin: " << plugin_name;
        return true;

    } catch (const std::exception& e) {
        emit_event(PluginEvent::ERROR, plugin_name,
                   "Unload failed: " + std::string(e.what()));
        SHIELD_LOG_ERROR << "Failed to unload plugin " << plugin_name << ": "
                         << e.what();
        return false;
    }
}

void PluginManager::unload_all_plugins() {
    // Unload in reverse order
    std::vector<std::string> plugin_names;
    plugin_names.reserve(loaded_plugins_.size());

    for (const auto& pair : loaded_plugins_) {
        plugin_names.push_back(pair.first);
    }

    // Reverse the order for safe unloading
    std::reverse(plugin_names.begin(), plugin_names.end());

    for (const auto& plugin_name : plugin_names) {
        unload_plugin(plugin_name);
    }
}

const PluginDescriptor* PluginManager::get_plugin(
    const std::string& plugin_name) const {
    auto it = loaded_plugins_.find(plugin_name);
    return it != loaded_plugins_.end() ? it->second.get() : nullptr;
}

std::vector<std::string> PluginManager::get_loaded_plugins() const {
    std::vector<std::string> names;
    names.reserve(loaded_plugins_.size());

    for (const auto& pair : loaded_plugins_) {
        names.push_back(pair.first);
    }

    return names;
}

bool PluginManager::is_plugin_loaded(const std::string& plugin_name) const {
    return loaded_plugins_.find(plugin_name) != loaded_plugins_.end();
}

void PluginManager::set_event_callback(PluginEventCallback callback) {
    event_callback_ = std::move(callback);
}

std::vector<IStarter*> PluginManager::get_plugin_starters() const {
    std::vector<IStarter*> starters;
    starters.reserve(loaded_plugins_.size());

    for (const auto& pair : loaded_plugins_) {
        if (pair.second->starter) {
            starters.push_back(pair.second->starter.get());
        }
    }

    return starters;
}

bool PluginManager::load_plugin_from_library(
    const boost::filesystem::path& library_path,
    const std::string& plugin_name) {
    emit_event(PluginEvent::LOADING, plugin_name,
               "From: " + library_path.string());

    try {
        // Create plugin descriptor
        auto descriptor = std::make_unique<PluginDescriptor>();
        descriptor->name = plugin_name;
        descriptor->library_path = library_path;

        // Load library using boost::dll
        descriptor->library.load(library_path);

        if (!descriptor->library.is_loaded()) {
            emit_event(PluginEvent::ERROR, plugin_name,
                       "Failed to load library");
            return false;
        }

        // Validate required symbols
        if (!validate_plugin_library(descriptor->library)) {
            emit_event(PluginEvent::ERROR, plugin_name,
                       "Invalid plugin library");
            return false;
        }

        // Extract plugin info
        auto plugin_info = extract_plugin_info(descriptor->library);
        descriptor->version =
            plugin_info.version ? plugin_info.version : "unknown";
        descriptor->description =
            plugin_info.description ? plugin_info.description : "";
        descriptor->author =
            plugin_info.author ? plugin_info.author : "unknown";

        // Extract dependencies
        if (plugin_info.dependencies) {
            for (const char** dep = plugin_info.dependencies; *dep != nullptr;
                 ++dep) {
                descriptor->dependencies.emplace_back(*dep);
            }
        }

        // Create starter instance using boost::dll symbol import
        auto create_func =
            descriptor->library.get<PluginCreateFunction>("create_starter");

        descriptor->starter = create_func();

        if (!descriptor->starter) {
            emit_event(PluginEvent::ERROR, plugin_name,
                       "Failed to create starter instance");
            return false;
        }

        // Store plugin
        loaded_plugins_[plugin_name] = std::move(descriptor);

        emit_event(PluginEvent::LOADED, plugin_name);
        SHIELD_LOG_INFO << "Successfully loaded plugin: " << plugin_name;
        return true;

    } catch (const std::exception& e) {
        emit_event(PluginEvent::ERROR, plugin_name,
                   "Load failed: " + std::string(e.what()));
        SHIELD_LOG_ERROR << "Failed to load plugin " << plugin_name << ": "
                         << e.what();
        return false;
    }
}

bool PluginManager::validate_plugin_library(
    const PluginLibrary& library) const {
    // Check for required symbols using boost::dll
    return library.has("create_starter") && library.has("get_plugin_info");
}

PluginInfo PluginManager::extract_plugin_info(
    const PluginLibrary& library) const {
    try {
        auto info_func = library.get<PluginInfoFunction>("get_plugin_info");
        return info_func();
    } catch (const std::exception& e) {
        SHIELD_LOG_WARN << "Failed to extract plugin info: " << e.what();
    }
    return PluginInfo{};  // Return empty info
}

void PluginManager::emit_event(PluginEvent event,
                               const std::string& plugin_name,
                               const std::string& message) {
    if (event_callback_) {
        event_callback_(event, plugin_name, message);
    }
}

std::string PluginManager::get_library_extension() const {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::vector<std::string> PluginManager::resolve_plugin_load_order() const {
    std::unordered_map<std::string, std::vector<std::string>> dependency_graph;
    for (const auto& [plugin_name, library_path] : discovered_plugins_) {
        auto info = read_plugin_info(library_path);
        std::vector<std::string> dependencies;
        if (info.dependencies) {
            for (const char** dep = info.dependencies; *dep != nullptr; ++dep) {
                dependencies.emplace_back(*dep);
            }
        }
        dependency_graph.emplace(plugin_name, std::move(dependencies));
    }

    std::vector<std::string> order;
    order.reserve(dependency_graph.size());
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;

    std::vector<std::string> plugin_names;
    plugin_names.reserve(dependency_graph.size());
    for (const auto& [plugin_name, _] : dependency_graph) {
        plugin_names.push_back(plugin_name);
    }
    std::sort(plugin_names.begin(), plugin_names.end());

    for (const auto& plugin_name : plugin_names) {
        if (visited.find(plugin_name) == visited.end()) {
            visit_plugin_dependency(plugin_name, dependency_graph, visiting,
                                    visited, order);
        }
    }

    return order;
}

void PluginManager::visit_plugin_dependency(
    const std::string& plugin_name,
    const std::unordered_map<std::string, std::vector<std::string>>&
        dependency_graph,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& visited,
    std::vector<std::string>& order) const {
    if (visited.find(plugin_name) != visited.end()) {
        return;
    }
    if (visiting.find(plugin_name) != visiting.end()) {
        throw std::runtime_error("Circular plugin dependency detected: " +
                                 plugin_name);
    }

    auto graph_it = dependency_graph.find(plugin_name);
    if (graph_it == dependency_graph.end()) {
        throw std::runtime_error("Unknown discovered plugin: " + plugin_name);
    }

    visiting.insert(plugin_name);
    for (const auto& dependency : graph_it->second) {
        if (dependency_graph.find(dependency) == dependency_graph.end()) {
            throw std::runtime_error("Plugin '" + plugin_name +
                                     "' depends on undiscovered plugin '" +
                                     dependency + "'");
        }
        visit_plugin_dependency(dependency, dependency_graph, visiting, visited,
                                order);
    }
    visiting.erase(plugin_name);
    visited.insert(plugin_name);
    order.push_back(plugin_name);
}

PluginInfo PluginManager::read_plugin_info(
    const boost::filesystem::path& library_path) const {
    PluginLibrary library;
    library.load(library_path);

    if (!validate_plugin_library(library)) {
        throw std::runtime_error("Invalid plugin library: " +
                                 library_path.string());
    }

    return extract_plugin_info(library);
}

}  // namespace shield::core
