// [SHIELD_PLUGIN] Plugin system v1 — host API, error, context.
//
// The host fills a shield_host_api_v1 table and hands it to each plugin via
// create(). Plugins use these callbacks to log, read their config, fetch
// resolved dependencies, register Lua paths, and report errors — without
// linking against the host. There is NO global plugin lookup: a plugin can
// only reach the dependencies it declared in its manifest requires[].

#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log levels matching shield::log.
enum shield_log_level {
    SHIELD_LOG_DEBUG = 0,
    SHIELD_LOG_INFO  = 1,
    SHIELD_LOG_WARN  = 2,
    SHIELD_LOG_ERROR = 3,
};

// Structured error. All plugin-system failure paths produce one of these.
// `code` is a stable dotted identifier (e.g. "plugin.abi.mismatch").
// String fields are NULL-terminated UTF-8 and may be NULL when not applicable.
struct shield_error_v1 {
    const char* code;          // stable code, never NULL on failure
    const char* message;       // human-readable detail
    const char* hint;          // optional remediation hint
    const char* package_id;    // package involved, may be NULL
    const char* instance_id;   // instance involved, may be NULL
    const char* phase;         // scan|catalog|plan|resolve|load|create|start|lua_register
};

// Opaque per-instance context. The host allocates one per instance and the
// plugin treats it as a black token passed back to config_get / dependency /
// lua_add_path. Layout is host-private; plugins must not inspect its contents.
struct shield_plugin_context_v1;

// Host function table. The host populates every slot before calling create().
struct shield_host_api_v1 {
    // Emit a log line tagged with the package and instance.
    void (*log)(enum shield_log_level level,
                const char* package_id,
                const char* instance_id,
                const char* message);

    // Report a structured error. The host logs it and may escalate (e.g.
    // fail a required instance). Does not return.
    void (*report_error)(const struct shield_error_v1* err);

    // Look up a config value by dot-path within THIS instance's validated
    // config (e.g. "host" or "pool.size"). Returns a host-owned string
    // (JSON fragment for objects/arrays, scalar text otherwise) valid until
    // the next config_get call on the same thread, or NULL if absent.
    const char* (*config_get)(struct shield_plugin_context_v1* ctx,
                              const char* path);

    // Fetch a resolved dependency by its require-name (as declared in this
    // instance's manifest requires[]) and the interface to retrieve. The
    // host verifies the name is declared and the target instance provides
    // the interface. Returns the dependency's interface vtable pointer, or
    // NULL if the dependency is absent (optional deps may legitimately be).
    const void* (*dependency)(struct shield_plugin_context_v1* ctx,
                              const char* name,
                              const char* interface_name);

    // Return the host's lua_State*. NULL if the host is running without a
    // Lua runtime (pure C++ mode). During register_lua this is non-NULL.
    struct lua_State* (*lua_state)(struct shield_plugin_context_v1* ctx);

    // Append a path to Lua's package.path (is_cpath == 0) or package.cpath
    // (is_cpath == 1). Relative paths resolve against the plugin package
    // root directory. Multiple calls accumulate. Returns 0 on success.
    // Typical use: ctx->host_api->lua_add_path(ctx, "lua/?.lua", 0).
    int (*lua_add_path)(struct shield_plugin_context_v1* ctx,
                        const char* path,
                        int is_cpath);

    // Resolve a logical name from plugins.bindings to the target instance id.
    // Plugins use this for Lua callable namespaces so business code passes
    // binding names (e.g. "database.default") instead of deployment instance
    // ids (e.g. "db.main"). Returns a host-owned string valid until the next
    // binding_instance_id call on the same thread, or NULL if absent.
    const char* (*binding_instance_id)(struct shield_plugin_context_v1* ctx,
                                       const char* binding);
};

#ifdef __cplusplus
}  // extern "C"
#endif
