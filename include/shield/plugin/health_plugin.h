// [SHIELD_PLUGIN] Health check plugin C ABI
//
// Stable C interface for health check endpoints (Kubernetes liveness/
// readiness probes, load balancer health checks, etc.).
//
// Most single-node deployments don't need health checks. This plugin is
// only loaded when explicitly enabled via config.
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_HEALTH, vtable → shield_health_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_HEALTH_ABI_VERSION 1

// Health check result.
enum shield_health_status {
    SHIELD_HEALTH_OK       = 0,  // Service is healthy
    SHIELD_HEALTH_DEGRADED = 1,  // Service is degraded but functional
    SHIELD_HEALTH_FAIL     = 2,  // Service is unhealthy
};

struct shield_health_check_result {
    enum shield_health_status status;
    const char* check_name;      // e.g. "database", "redis", "custom"
    const char* message;         // human-readable description
    int64_t latency_ms;          // check duration, 0 = instant
};

struct shield_health_plugin {
    uint32_t abi_version;
    const char* name;            // "http", "tcp", "custom"
    const char* version;

    // Initialize the health check endpoint.
    int (*init)(const char* config_json, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Register a health check function.
    // The check function is called periodically and on probe requests.
    int (*register_check)(const char* name,
                          int (*check)(struct shield_health_check_result* out,
                                       void* user_data),
                          void* user_data);

    // Run all registered checks and return overall status.
    int (*check_all)(struct shield_health_check_result* results,
                     int max_results,
                     int* out_count);

    // Get overall status (aggregated from all checks).
    enum shield_health_status (*get_status)(void);

    // Start serving health endpoint (e.g. HTTP /health, /ready).
    int (*start_endpoint)(const char* bind_address, int port);

    // Stop serving health endpoint.
    void (*stop_endpoint)(void);

    // Memory
    void (*free_result)(struct shield_health_check_result* result);
};

// Entry point exported by every health plugin DLL.
#ifdef _WIN32
#define SHIELD_HEALTH_EXPORT __declspec(dllexport)
#else
#define SHIELD_HEALTH_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_HEALTH_EXPORT
const struct shield_health_plugin* shield_health_plugin_api(void);

#ifdef __cplusplus
}
#endif
