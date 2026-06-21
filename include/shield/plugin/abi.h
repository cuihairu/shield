// [SHIELD_PLUGIN] Plugin system v1 — core ABI.
//
// Stable C entry point: every plugin shared library exports
// shield_plugin_get_v1(), returning a pointer to a static
// shield_plugin_abi_v1 table. The host validates abi_version / struct_size /
// package_id before trusting the plugin.
//
// Interface-name based: there is no global plugin-type enum. Each instance
// exposes its interfaces by name via get_interface().
//
// ABI stability rules:
//   - Appending fields to the END of these structs (and bumping handling) is
//     allowed; host uses struct_size to detect support.
//   - Reordering, removing, or repurposing existing fields is NOT allowed.
//   - All strings crossing the boundary are NULL-terminated UTF-8.
//
// Threading:
//   - create/start/shutdown are called from the host bootstrap thread.
//   - Interface vtable methods may be called concurrently from worker
//     threads; the plugin must document its own synchronization needs.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #define SHIELD_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define SHIELD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// Bumped on any backward-incompatible layout change of the structs below.
#define SHIELD_PLUGIN_ABI_VERSION 1

// Forward declarations — full definitions live in host_api.h (error/context)
// and host_api.h (host_api). Keen separation keeps abi.h minimal for plugin
// authors who only need the entry contract.
struct shield_host_api_v1;
struct shield_error_v1;
struct shield_plugin_context_v1;

// Unified instance shell. The host only ever interacts with a plugin
// instance through these three function pointers — it never inspects the
// plugin's internal state. The concrete struct embedded behind this shell
// is plugin-defined.
struct shield_plugin_instance_v1 {
    uint32_t struct_size;          // == sizeof(plugin's concrete instance)
    const char* instance_id;       // matches the id in plugins.instances[]

    // Return the vtable for a requested interface, or NULL if this instance
    // does not provide it. `err` receives a structured error when NULL is
    // returned due to a real failure (vs. simply not providing the iface).
    const void* (*get_interface)(struct shield_plugin_instance_v1* self,
                                 const char* interface_name,
                                 struct shield_error_v1* err);

    // Start the instance (open resources, connect, etc.). Called after all
    // dependencies have been started. Returns 0 on success.
    int (*start)(struct shield_plugin_instance_v1* self,
                 struct shield_error_v1* err);

    // Stop the instance and release all resources. Safe to call once.
    void (*shutdown)(struct shield_plugin_instance_v1* self);
};

// Arguments passed to the plugin's create() entry. The host fills these in
// after resolving config and dependencies.
struct shield_plugin_create_args_v1 {
    // Host function table — logging, error reporting, config lookup,
    // dependency access. Valid for the lifetime of the instance.
    const struct shield_host_api_v1* host_api;

    // Opaque per-instance context. The plugin stores this pointer and passes
    // it back to host_api callbacks (config_get / dependency) so the host can
    // identify which instance is calling. Host-owned, host-allocated.
    struct shield_plugin_context_v1* ctx;

    // Instance id from plugins.instances[].config (NOT the package id).
    const char* instance_id;

    // Validated instance config as a JSON string. Already checked against
    // config_schema with defaults applied. May be "{}".
    const char* config_json;
};

// The per-package ABI table returned by shield_plugin_get_v1().
struct shield_plugin_abi_v1 {
    uint32_t abi_version;          // must == SHIELD_PLUGIN_ABI_VERSION
    uint32_t struct_size;          // host checks >= minimum it knows
    const char* package_id;        // must match plugin.json "id"
    const char* package_version;   // human-readable, e.g. "1.0.0"

    // Construct an instance. On success returns 0 and writes *out. On failure
    // returns non-zero and fills `err` (if non-NULL).
    int (*create)(const struct shield_plugin_create_args_v1* args,
                  struct shield_plugin_instance_v1** out,
                  struct shield_error_v1* err);
};

// The single entry point every plugin shared library exports.
SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void);

#ifdef __cplusplus
}  // extern "C"
#endif
