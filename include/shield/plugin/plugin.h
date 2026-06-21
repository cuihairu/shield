// [SHIELD_PLUGIN] Generic plugin C ABI
//
// Stable C interface for all Shield plugins. Each plugin is a shared library
// (.dll/.so/.dylib) that exports `shield_plugin_api()` returning a pointer
// to a static `shield_plugin_t` struct.
//
// Inspired by Node.js N-API: stable ABI, versioned, language-neutral.
//
// Usage:
//   1. Implement a shared library with the required entry point.
//   2. Place it next to the shield executable or in the plugin directory.
//   3. Shield discovers and loads plugins at startup.
//   4. Plugins register their capabilities via the function table.
//
// ABI stability rules:
//   - Appending new function pointers to the END of the vtable is allowed.
//   - Reordering, removing, or repurposing existing slots is NOT allowed.
//   - All strings crossing the boundary are NULL-terminated UTF-8.
//   - Memory allocated by the plugin must be freed via plugin callbacks.
//
// Threading:
//   - Plugin init/shutdown are called from the main thread.
//   - Plugin functions may be called from multiple threads concurrently.
//   - The plugin must handle its own synchronization if needed.

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define SHIELD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SHIELD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// ============================================================================
// Version & Type System
// ============================================================================

// Bump when the layout of shield_plugin_t changes in a
// backward-incompatible way.
#define SHIELD_PLUGIN_ABI_VERSION 1

// Plugin type identifiers. Each type defines a specific capability domain.
enum shield_plugin_type {
    SHIELD_PLUGIN_TYPE_DATABASE    = 0x01,  // Database backend
    SHIELD_PLUGIN_TYPE_TRANSPORT   = 0x02,  // Network transport (TCP/UDP/WebSocket)
    SHIELD_PLUGIN_TYPE_AUTH        = 0x03,  // Authentication provider
    SHIELD_PLUGIN_TYPE_CACHE       = 0x04,  // Cache backend (Redis/Memcached)
    SHIELD_PLUGIN_TYPE_QUEUE       = 0x05,  // Message queue (Redis pub/sub/RabbitMQ/NATS)
    SHIELD_PLUGIN_TYPE_STORAGE     = 0x06,  // Object storage (S3/GCS/local)
    SHIELD_PLUGIN_TYPE_METRIC      = 0x07,  // Metrics exporter (Prometheus/StatsD)
    SHIELD_PLUGIN_TYPE_HEALTH      = 0x08,  // Health check endpoint
    SHIELD_PLUGIN_TYPE_LOG         = 0x09,  // Log sink (stdout/file/remote)
    SHIELD_PLUGIN_TYPE_GATEWAY     = 0x0A,  // Gateway protocol (HTTP/WebSocket)
    SHIELD_PLUGIN_TYPE_LEADERBOARD = 0x0B,  // Leaderboard (Redis ZSET/DB/memory)
    SHIELD_PLUGIN_TYPE_MATCHMAKING = 0x0C,  // Matchmaking (ELO/MMR/skill-based)
    SHIELD_PLUGIN_TYPE_GAME        = 0x10,  // Game logic plugin (custom)
    SHIELD_PLUGIN_TYPE_USER        = 0xFF,  // User-defined plugin type
};

// ============================================================================
// Plugin Configuration
// ============================================================================

// Configuration key-value pair passed to plugin init.
struct shield_plugin_config_item {
    const char* key;
    const char* value;  // NULL if key has no value
};

// Configuration set passed to plugin init.
struct shield_plugin_config {
    const struct shield_plugin_config_item* items;
    int count;
};

// ============================================================================
// Plugin Capability Registration
// ============================================================================

// A capability that a plugin advertises to the host.
struct shield_plugin_capability {
    const char* name;       // e.g. "database.mysql", "transport.tcp"
    const char* version;    // e.g. "1.0.0"
    const char* description; // human-readable
};

// ============================================================================
// Host Context — how plugins call back into Shield
// ============================================================================

// Opaque handle to the Shield host runtime. Passed to plugin init().
// Plugins use this to access Shield services (logging, config, other plugins).
// The actual struct is defined by the host; plugins only use the pointer.
struct shield_host {
    int _reserved;  // ensures non-zero size
};
typedef struct shield_host* shield_host_t;

// Log levels matching Shield's logger.
enum shield_log_level {
    SHIELD_LOG_DEBUG = 0,
    SHIELD_LOG_INFO  = 1,
    SHIELD_LOG_WARN  = 2,
    SHIELD_LOG_ERROR = 3,
};

// Host function table. The host fills this struct and passes it to
// plugin init(). Plugins use these function pointers to call back
// into Shield without linking against it.
struct shield_host_api {
    // --- Logging ------------------------------------------------------------
    void (*log)(enum shield_log_level level,
                const char* plugin_name,
                const char* message);

    // --- Configuration ------------------------------------------------------
    // Get a config value by key. Returns NULL if not found.
    // The returned string is valid until next call.
    const char* (*get_config)(shield_host_t host, const char* key);

    // --- Plugin Registry ----------------------------------------------------
    // Find another plugin by name. Returns NULL if not loaded.
    const struct shield_plugin* (*find_plugin)(shield_host_t host,
                                               const char* name);

    // Get the vtable of another plugin (type-unsafe, caller must know the type).
    const void* (*get_plugin_vtable)(shield_host_t host, const char* name);

    // --- Lifecycle Hooks ----------------------------------------------------
    // Register a shutdown hook (called when Shield is shutting down).
    void (*register_shutdown_hook)(shield_host_t host,
                                   void (*hook)(void* user_data),
                                   void* user_data);

    // --- Error Reporting ----------------------------------------------------
    // Report a fatal plugin error. Shield will log and may initiate shutdown.
    void (*report_error)(shield_host_t host,
                         const char* plugin_name,
                         const char* error_code,
                         const char* message);
};

// ============================================================================
// Plugin Function Table
// ============================================================================

// The main plugin interface. Each plugin fills this struct with its
// implementation functions. NULL means "not implemented" (host must check).
struct shield_plugin {
    // --- Identity -----------------------------------------------------------
    uint32_t abi_version;       // must equal SHIELD_PLUGIN_ABI_VERSION
    enum shield_plugin_type type;  // plugin type
    const char* name;           // e.g. "mysql", "redis", "jwt-auth"
    const char* version;        // e.g. "1.0.0"
    const char* description;    // human-readable description
    const char* author;         // plugin author

    // --- Lifecycle ----------------------------------------------------------

    // Initialize the plugin. Called once at startup.
    // host: handle to the Shield runtime (for calling back into Shield).
    // host_api: function table for host services (logging, config, etc.).
    // config: plugin-specific configuration from YAML.
    // Returns 0 on success, non-zero on failure.
    // err_buf: optional buffer for error message (NULL-terminated).
    int (*init)(shield_host_t host,
                const struct shield_host_api* host_api,
                const struct shield_plugin_config* config,
                char* err_buf, int err_buf_size);

    // Shutdown the plugin. Called once at exit.
    // Must release all resources.
    void (*shutdown)(void);

    // --- Capability Registration --------------------------------------------

    // Get the number of capabilities this plugin provides.
    int (*capability_count)(void);

    // Get a capability by index (0-based).
    // Returns NULL if index >= capability_count().
    const struct shield_plugin_capability* (*get_capability)(int index);

    // --- Extension Points ---------------------------------------------------

    // Plugin-specific function table. The host casts this to the
    // appropriate type based on plugin->type.
    //
    // For DATABASE plugins: shield_db_plugin*
    // For TRANSPORT plugins: shield_transport_plugin* (future)
    // For AUTH plugins: shield_auth_plugin* (future)
    // etc.
    const void* vtable;
};

// ============================================================================
// Plugin Entry Point
// ============================================================================

// Entry point exported by every plugin DLL. Returns a pointer to a
// static, plugin-owned shield_plugin struct. The pointer is valid for
// the lifetime of the loaded module.
//
// If the plugin cannot initialize (e.g. missing system deps), it MAY
// return NULL. The host treats this as "plugin unavailable".
SHIELD_PLUGIN_EXPORT
const struct shield_plugin* shield_plugin_api(void);

// ============================================================================
// Plugin Manager Types (used by host, not by plugins)
// ============================================================================

// Plugin discovery info (not part of the plugin ABI, used internally).
struct shield_plugin_info {
    char name[64];
    char path[512];
    enum shield_plugin_type type;
    int loaded;
};

#ifdef __cplusplus
}  // extern "C"
#endif
