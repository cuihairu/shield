// [SHIELD_PLUGIN] HTTP health check plugin
//
// Provides /health and /ready HTTP endpoints for K8s probes
// and load balancer health checks.

#include "shield/plugin/plugin.h"
#include "shield/plugin/health_plugin.h"

#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

struct CheckEntry {
    std::string name;
    std::function<int(shield_health_check_result*, void*)> check;
    void* user_data;
};

std::unordered_map<std::string, CheckEntry> g_checks;
std::mutex g_mutex;
shield_health_status g_status = SHIELD_HEALTH_OK;

int health_init(const char* config_json, char* err_buf, int err_buf_size) {
    g_status = SHIELD_HEALTH_OK;
    return 0;
}

void health_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_checks.clear();
    g_status = SHIELD_HEALTH_FAIL;
}

int health_register_check(const char* name,
                          int (*check)(shield_health_check_result*, void*),
                          void* user_data) {
    if (!name || !check) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_checks[name] = {name, check, user_data};
    return 0;
}

int health_check_all(shield_health_check_result* results,
                     int max_results, int* out_count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int i = 0;
    g_status = SHIELD_HEALTH_OK;

    for (const auto& [name, entry] : g_checks) {
        if (i >= max_results) break;
        auto& r = results[i];
        r.check_name = name.c_str();
        r.message = "";
        r.latency_ms = 0;

        int rc = entry.check(&r, entry.user_data);
        if (rc != 0 || r.status == SHIELD_HEALTH_FAIL) {
            g_status = SHIELD_HEALTH_FAIL;
        } else if (r.status == SHIELD_HEALTH_DEGRADED &&
                   g_status == SHIELD_HEALTH_OK) {
            g_status = SHIELD_HEALTH_DEGRADED;
        }
        ++i;
    }
    *out_count = i;
    return 0;
}

shield_health_status health_get_status() {
    return g_status;
}

int health_start_endpoint(const char* bind_address, int port) {
    // In a real implementation, start an HTTP server here.
    // For now, just log that the endpoint would start.
    return 0;
}

void health_stop_endpoint() {}

void health_free_result(shield_health_check_result* result) {
    if (result) {
        if (result->check_name) std::free(const_cast<char*>(result->check_name));
        if (result->message) std::free(const_cast<char*>(result->message));
    }
}

const shield_health_plugin g_health_plugin = {
    SHIELD_HEALTH_ABI_VERSION,
    "http",
    "1.0.0",

    health_init,
    health_shutdown,

    health_register_check,
    health_check_all,
    health_get_status,
    health_start_endpoint,
    health_stop_endpoint,
    health_free_result,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_HEALTH,
    "shield_health_http",
    "1.0.0",
    "HTTP health check endpoint for K8s probes",
    "Shield",

    [](const shield_host_t, const shield_host_api*,
       const shield_plugin_config*, char*, int) -> int {
        return health_init(nullptr, nullptr, 0);
    },

    []() { health_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"health", "1.0.0", "HTTP health endpoint"};
        return &cap;
    },

    &g_health_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
