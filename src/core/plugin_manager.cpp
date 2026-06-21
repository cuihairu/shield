// [SHIELD_CORE] Plugin manager implementation
#include "shield/core/plugin_manager.hpp"

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/db_plugin.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Cross-platform dynamic library loader (same logic as shield_data's
// DynamicLibrary but self-contained to avoid private header dependency).
class PluginLibrary {
public:
    PluginLibrary() = default;
    ~PluginLibrary() { close(); }

    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;

    PluginLibrary(PluginLibrary&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    PluginLibrary& operator=(PluginLibrary&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    static PluginLibrary load(const std::string& path, std::string& error) {
        PluginLibrary lib;
#ifdef _WIN32
        lib.handle_ = LoadLibraryA(path.c_str());
        if (!lib.handle_) {
            error = "LoadLibrary failed: " + std::to_string(GetLastError());
        }
#else
        lib.handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib.handle_) {
            error = dlerror();
        }
#endif
        return lib;
    }

    bool is_loaded() const { return handle_ != nullptr; }

    void* resolve(const char* symbol) const {
        if (!handle_) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(
            static_cast<HMODULE>(handle_), symbol));
#else
        return dlsym(handle_, symbol);
#endif
    }

private:
    void close() {
        if (handle_) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
            handle_ = nullptr;
        }
    }

    void* handle_ = nullptr;
};

}  // namespace

namespace shield::core {

namespace fs = std::filesystem;

struct PluginEntry {
    shield_plugin plugin;           // copy of the plugin struct
    const shield_plugin* original;  // pointer into the DLL
    PluginLibrary lib;
    std::string path;
    bool initialized = false;       // true after successful init
};

struct PluginManager::Impl {
    std::unordered_map<std::string, PluginEntry> plugins;
    std::vector<std::string> load_order;  // init order = shutdown reverse
    std::unordered_map<std::string, std::string> discovered_paths;
    mutable std::mutex mutex;

    // Host API function table (passed to plugins during init).
    shield_host_api host_api;
    // Opaque host handle (pointer to self, used by plugins to identify the host).
    struct shield_host opaque_host;

    Impl() {
        // Fill the host API function table.
        host_api.log = [](enum shield_log_level level, const char* plugin_name,
                          const char* message) {
            auto& log = shield::log::get_logger(plugin_name ? plugin_name : "plugin");
            std::string msg = message ? message : "";
            switch (level) {
                case SHIELD_LOG_DEBUG: SHIELD_LOG_DEBUG(log, msg); break;
                case SHIELD_LOG_INFO:  SHIELD_LOG_INFO(log, msg); break;
                case SHIELD_LOG_WARN:  SHIELD_LOG_WARNING(log, msg); break;
                case SHIELD_LOG_ERROR: SHIELD_LOG_ERROR(log, msg); break;
            }
        };

        host_api.get_config = [](shield_host_t, const char* key) -> const char* {
            if (!key) return nullptr;
            auto& cfg = shield::config::global_config();
            static thread_local std::string value;
            value = cfg.get_string(key, "");
            return value.empty() ? nullptr : value.c_str();
        };

        host_api.find_plugin = [](shield_host_t host,
                                   const char* name) -> const shield_plugin* {
            if (!host || !name) return nullptr;
            auto* self = reinterpret_cast<PluginManager::Impl*>(
                reinterpret_cast<char*>(host) - offsetof(PluginManager::Impl, opaque_host));
            std::lock_guard<std::mutex> lock(self->mutex);
            auto it = self->plugins.find(name);
            return it != self->plugins.end() ? &it->second.plugin : nullptr;
        };

        host_api.get_plugin_vtable = [](shield_host_t host,
                                         const char* name) -> const void* {
            if (!host || !name) return nullptr;
            auto* self = reinterpret_cast<PluginManager::Impl*>(
                reinterpret_cast<char*>(host) - offsetof(PluginManager::Impl, opaque_host));
            auto it = self->plugins.find(name);
            if (it == self->plugins.end()) return nullptr;
            return it->second.plugin.vtable;
        };

        host_api.register_shutdown_hook = [](shield_host_t,
                                              void (*)(void*),
                                              void*) {
            // Phase 2: store hooks and call them during shutdown.
        };

        host_api.report_error = [](shield_host_t,
                                    const char* plugin_name,
                                    const char* error_code,
                                    const char* message) {
            auto& log = shield::log::get_logger(plugin_name ? plugin_name : "plugin");
            SHIELD_LOG_ERROR(log, std::string("[") + (error_code ? error_code : "unknown") +
                            "] " + (message ? message : ""));
        };
    }

    static shield_plugin_type name_to_type(const std::string& name) {
        if (name.find("db") != std::string::npos ||
            name.find("mysql") != std::string::npos ||
            name.find("postgres") != std::string::npos ||
            name.find("sqlite") != std::string::npos) {
            return SHIELD_PLUGIN_TYPE_DATABASE;
        }
        if (name.find("redis") != std::string::npos ||
            name.find("cache") != std::string::npos) {
            return SHIELD_PLUGIN_TYPE_CACHE;
        }
        if (name.find("auth") != std::string::npos) {
            return SHIELD_PLUGIN_TYPE_AUTH;
        }
        return SHIELD_PLUGIN_TYPE_USER;
    }

    static std::string type_to_string(shield_plugin_type type) {
        switch (type) {
            case SHIELD_PLUGIN_TYPE_DATABASE:  return "database";
            case SHIELD_PLUGIN_TYPE_TRANSPORT: return "transport";
            case SHIELD_PLUGIN_TYPE_AUTH:      return "auth";
            case SHIELD_PLUGIN_TYPE_CACHE:     return "cache";
            case SHIELD_PLUGIN_TYPE_STORAGE:   return "storage";
            case SHIELD_PLUGIN_TYPE_METRIC:    return "metric";
            case SHIELD_PLUGIN_TYPE_LOG:       return "log";
            case SHIELD_PLUGIN_TYPE_GATEWAY:   return "gateway";
            case SHIELD_PLUGIN_TYPE_GAME:      return "game";
            case SHIELD_PLUGIN_TYPE_USER:      return "user";
        }
        return "unknown";
    }
};

PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {}

PluginManager::~PluginManager() {
    shutdown_all();
}

void PluginManager::discover(const std::string& plugin_dir) {
    auto& log = shield::log::get_logger("plugin");

    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        SHIELD_LOG_WARNING(log, "Plugin directory not found: " + plugin_dir);
        return;
    }

    int found = 0;
    for (const auto& entry : fs::directory_iterator(plugin_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        std::string ext = entry.path().extension().string();

        // Match plugin shared libraries.
        // Windows: LoadLibraryA loads any PE file regardless of extension.
        // Linux/macOS: dlopen uses the extension to identify shared libraries.
        bool is_plugin = false;
        if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
            is_plugin = true;
        }
        // Also accept extensionless files on Linux/macOS (some build systems
        // strip the extension).
        if (!is_plugin && ext.empty()) {
#ifndef _WIN32
            // Try dlopen to check if it's a valid shared library.
            // Skip for now - require an extension.
#endif
        }

        if (!is_plugin) continue;

        // Extract plugin name from filename (remove extension).
        std::string name = entry.path().stem().string();
        // Strip "lib" prefix on Linux/macOS (libshield_redis.so → shield_redis).
#ifndef _WIN32
        if (name.substr(0, 3) == "lib") {
            name = name.substr(3);
        }
#endif
        impl_->discovered_paths[name] = entry.path().string();
        ++found;

        SHIELD_LOG_INFO(log, "Discovered plugin: " + name + " at " +
                        entry.path().string());
    }

    SHIELD_LOG_INFO(log, "Discovered " + std::to_string(found) + " plugins in " +
                    plugin_dir);
}

bool PluginManager::load(const std::string& path, std::string& error) {
    auto& log = shield::log::get_logger("plugin");

    // Load the DLL.
    std::string lib_error;
    auto lib = PluginLibrary::load(path, lib_error);
    if (!lib.is_loaded()) {
        error = "Failed to load plugin: " + path + " (" + lib_error + ")";
        SHIELD_LOG_ERROR(log, error);
        return false;
    }

    // Try shield_plugin_api() first (new generic entry point).
    using PluginApiFn = const shield_plugin* (*)();
    auto api_fn = reinterpret_cast<PluginApiFn>(
        lib.resolve("shield_plugin_api"));

    // Fallback: try shield_db_plugin_api() for legacy database plugins.
    // If found, wrap it in a shield_plugin struct.
    const shield_plugin* plugin = nullptr;
    shield_plugin db_wrapper{};

    if (api_fn) {
        plugin = api_fn();
    } else {
        // Try legacy DB entry point.
        using DbApiFn = const shield_db_plugin* (*)();
        auto db_api_fn = reinterpret_cast<DbApiFn>(
            lib.resolve("shield_db_plugin_api"));
        if (db_api_fn) {
            const shield_db_plugin* db_plugin = db_api_fn();
            if (db_plugin) {
                // Wrap in a shield_plugin struct.
                db_wrapper.abi_version = SHIELD_PLUGIN_ABI_VERSION;
                db_wrapper.type = SHIELD_PLUGIN_TYPE_DATABASE;
                db_wrapper.name = db_plugin->name ? db_plugin->name : "unknown_db";
                db_wrapper.version = db_plugin->version ? db_plugin->version : "";
                db_wrapper.description = "Database plugin (legacy DB ABI)";
                db_wrapper.author = "";
                db_wrapper.init = nullptr;
                db_wrapper.shutdown = nullptr;
                db_wrapper.capability_count = nullptr;
                db_wrapper.get_capability = nullptr;
                db_wrapper.vtable = db_plugin;
                plugin = &db_wrapper;
                SHIELD_LOG_INFO(log, "Loaded legacy DB plugin via shield_db_plugin_api: " +
                                path);
            }
        }
    }

    if (!plugin) {
        error = "Plugin missing shield_plugin_api or shield_db_plugin_api entry point: " + path;
        SHIELD_LOG_ERROR(log, error);
        return false;
    }
    if (!plugin) {
        error = "Plugin returned NULL from shield_plugin_api: " + path;
        SHIELD_LOG_ERROR(log, error);
        return false;
    }

    // Validate ABI version.
    if (plugin->abi_version != SHIELD_PLUGIN_ABI_VERSION) {
        error = "Plugin ABI version mismatch: expected " +
                std::to_string(SHIELD_PLUGIN_ABI_VERSION) +
                ", got " + std::to_string(plugin->abi_version) +
                " (" + path + ")";
        SHIELD_LOG_ERROR(log, error);
        return false;
    }

    // Check for duplicate.
    std::string name = plugin->name ? plugin->name : "unknown";
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->plugins.find(name) != impl_->plugins.end()) {
            error = "Plugin already loaded: " + name;
            SHIELD_LOG_WARNING(log, error);
            return false;
        }

        // Store plugin entry.
        PluginEntry entry;
        entry.plugin = *plugin;  // copy the struct
        entry.original = plugin;
        entry.lib = std::move(lib);
        entry.path = path;

        impl_->plugins[name] = std::move(entry);
        impl_->load_order.push_back(name);
    }

    SHIELD_LOG_INFO(log, "Loaded plugin: " + name + " v" +
                    (plugin->version ? plugin->version : "?") +
                    " [" + impl_->type_to_string(plugin->type) + "]");
    return true;
}

bool PluginManager::load_by_name(const std::string& name, std::string& error) {
    auto it = impl_->discovered_paths.find(name);
    if (it == impl_->discovered_paths.end()) {
        error = "Plugin not discovered: " + name;
        return false;
    }
    return load(it->second, error);
}

bool PluginManager::init_all(
    const std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>>& config_map,
    std::string& error) {

    auto& log = shield::log::get_logger("plugin");

    for (const auto& name : impl_->load_order) {
        auto it = impl_->plugins.find(name);
        if (it == impl_->plugins.end()) continue;

        auto& entry = it->second;
        if (!entry.plugin.init) {
            // No init function — treat as success.
            SHIELD_LOG_INFO(log, "Plugin " + name + " has no init, skipping");
            continue;
        }

        // Build config for this plugin.
        auto config_it = config_map.find(name);
        std::vector<shield_plugin_config_item> items;
        if (config_it != config_map.end()) {
            for (const auto& [key, value] : config_it->second) {
                items.push_back({key.c_str(), value.c_str()});
            }
        }

        shield_plugin_config config;
        config.items = items.data();
        config.count = static_cast<int>(items.size());

        char err_buf[512] = {0};
        int rc = entry.plugin.init(&impl_->opaque_host, &impl_->host_api,
                                   &config, err_buf, sizeof(err_buf));
        if (rc != 0) {
            error = "Plugin init failed: " + name + " (" +
                    std::string(err_buf) + ")";
            SHIELD_LOG_ERROR(log, error);
            return false;
        }

        entry.initialized = true;
        SHIELD_LOG_INFO(log, "Initialized plugin: " + name);
    }

    SHIELD_LOG_INFO(log, "All plugins initialized (" +
                    std::to_string(impl_->plugins.size()) + ")");
    return true;
}

void PluginManager::shutdown_all() {
    auto& log = shield::log::get_logger("plugin");

    // Shutdown in reverse order.
    for (auto it = impl_->load_order.rbegin(); it != impl_->load_order.rend(); ++it) {
        auto plugin_it = impl_->plugins.find(*it);
        if (plugin_it == impl_->plugins.end()) continue;

        auto& entry = plugin_it->second;
        if (entry.plugin.shutdown) {
            entry.plugin.shutdown();
            SHIELD_LOG_INFO(log, "Shutdown plugin: " + *it);
        }
    }

    impl_->plugins.clear();
    impl_->load_order.clear();
}

const shield_plugin* PluginManager::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->plugins.find(name);
    if (it == impl_->plugins.end()) return nullptr;
    return &it->second.plugin;
}

std::vector<const shield_plugin*> PluginManager::by_type(
    shield_plugin_type type) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<const shield_plugin*> result;
    for (const auto& [name, entry] : impl_->plugins) {
        if (entry.plugin.type == type) {
            result.push_back(&entry.plugin);
        }
    }
    return result;
}

std::vector<PluginInfo> PluginManager::list() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<PluginInfo> result;
    for (const auto& [name, entry] : impl_->plugins) {
        PluginInfo info;
        info.name = name;
        info.version = entry.plugin.version ? entry.plugin.version : "";
        info.description = entry.plugin.description ? entry.plugin.description : "";
        info.path = entry.path;
        info.type = entry.plugin.type;
        info.initialized = entry.initialized;
        result.push_back(std::move(info));
    }
    return result;
}

bool PluginManager::is_loaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->plugins.find(name) != impl_->plugins.end();
}

size_t PluginManager::count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->plugins.size();
}

}  // namespace shield::core
