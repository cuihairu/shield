// [SHIELD_PLUGIN] shield.health.v1 interface.
//
// Health check endpoint provider (k8s liveness/readiness probes).
// connect() starts the endpoint and returns a session handle.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_HEALTH_INTERFACE "shield.health.v1"

struct shield_health_session;  // opaque, plugin-defined

enum shield_health_status {
    SHIELD_HEALTH_OK       = 0,
    SHIELD_HEALTH_DEGRADED = 1,
    SHIELD_HEALTH_FAIL     = 2,
};

struct shield_health_check_result {
    enum shield_health_status status;
    const char* check_name;
    const char* message;
    int64_t latency_ms;
};

struct shield_health_config {
    const char* bind_address;
    int port;
    const char* liveness_path;   // "/health"
    const char* readiness_path;  // "/ready"
    const char* extra_json;
};

struct shield_health_v1 {
    static constexpr const char* interface_name = SHIELD_HEALTH_INTERFACE;

    uint32_t struct_size;
    const char* name;            // "http" | "tcp" | ...
    const char* version;

    struct shield_health_session* (*connect)(
        const struct shield_health_config* cfg,
        char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_health_session* session);

    int (*register_check)(struct shield_health_session* session,
                          const char* name,
                          int (*check)(struct shield_health_check_result* out,
                                       void* user_data),
                          void* user_data);
    int (*check_all)(struct shield_health_session* session,
                     struct shield_health_check_result* results,
                     int max_results, int* out_count);
    enum shield_health_status (*get_status)(struct shield_health_session* session);

    void (*free_result)(struct shield_health_check_result* result);
};

#ifdef __cplusplus
}
#endif
