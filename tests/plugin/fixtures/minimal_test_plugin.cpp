// Minimal test plugin used by test_plugin_host / test_plugin_e2e to verify the
// load/create/start pipeline end-to-end without depending on sqlite/mysql.
#include <atomic>
#include <cstring>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"

namespace {

constexpr const char* kTestInterface = "minimal.test.iface";

struct minimal_test_interface_v1 {
    uint32_t struct_size;
    int marker;
};

struct minimal_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string instance_id;
};

std::atomic<int> g_started_count{0};
std::atomic<int> g_shutdown_count{0};

const minimal_test_interface_v1& test_vtable() {
    static const minimal_test_interface_v1 v{sizeof(minimal_test_interface_v1),
                                             0x5A17};
    return v;
}

bool config_true(minimal_instance* inst, const char* key) {
    if (!inst || !inst->host || !inst->host->config_get) return false;
    const char* v = inst->host->config_get(inst->ctx, key);
    return v && std::strcmp(v, "true") == 0;
}

// Canned pool stats for testing collect_pool_stats. The instance config
// switches behavior: "stats_unavailable" / "stats_unsupported" /
// "stats_unknown_rc" / "stats_error" exercise the return-code mapping;
// "stats_vtable_short" / "stats_no_fn" serve structurally invalid vtables
// (collect_pool_stats must skip them); "stats_vtable_null" makes
// get_interface return NULL (start must fail when the manifest declares the
// interface).
int minimal_pool_get_stats(struct shield_plugin_instance_v1* self,
                           struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<minimal_instance*>(self);
    if (!out) return -1;
    if (config_true(inst, "stats_unavailable")) {
        return 1;  // unavailable
    }
    if (config_true(inst, "stats_unsupported")) {
        return 2;  // unsupported_state
    }
    if (config_true(inst, "stats_unknown_rc")) {
        return 7;  // unrecognized positive code -> unsupported_state
    }
    if (config_true(inst, "stats_error")) {
        return -1;  // internal_error
    }
    out->max_size = 4;
    out->size = 2;
    out->idle = 1;
    out->in_use = 1;
    out->waiters = 0;
    out->acquire_timeout_total = 0;
    out->acquire_total = 10;
    out->create_total = 2;
    out->destroy_total = 0;
    out->eviction_total = 0;
    out->health_check_failures_total = 0;
    out->last_error_epoch_ms = 0;
    return 0;
}

const shield_pool_stats_v1& pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &minimal_pool_get_stats,
    };
    return v;
}

// Deliberately undersized vtable: struct_size covers nothing past the
// struct_size field itself.
const shield_pool_stats_v1& pool_stats_vtable_short() {
    static shield_pool_stats_v1 v = pool_stats_vtable();
    v.struct_size = sizeof(uint32_t);
    return v;
}

// Vtable with a null get_stats fn.
const shield_pool_stats_v1& pool_stats_vtable_no_fn() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        nullptr,
    };
    return v;
}

void fill_error(shield_error_v1* err, const char* code, const char* message,
                const char* phase = "start") {
    if (!err) return;
    err->code = code;
    err->message = message;
    err->phase = phase;
}

}  // namespace

static int minimal_create(const struct shield_plugin_create_args_v1* args,
                          struct shield_plugin_instance_v1** out,
                          struct shield_error_v1* err) {
    (void)err;
    auto* inst = new minimal_instance;
    inst->host = args ? args->host_api : nullptr;
    inst->ctx = args ? args->ctx : nullptr;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";
    inst->shell.struct_size = sizeof(minimal_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, kTestInterface) == 0) {
            return &test_vtable();
        }
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0) {
            auto* inst = reinterpret_cast<minimal_instance*>(self);
            if (config_true(inst, "stats_vtable_null")) {
                return nullptr;
            }
            if (config_true(inst, "stats_vtable_short")) {
                return &pool_stats_vtable_short();
            }
            if (config_true(inst, "stats_no_fn")) {
                return &pool_stats_vtable_no_fn();
            }
            return &pool_stats_vtable();
        }
        return nullptr;
    };
    inst->shell.start = [](struct shield_plugin_instance_v1* self,
                           struct shield_error_v1* err) {
        auto* inst = reinterpret_cast<minimal_instance*>(self);
        if (config_true(inst, "start_fail")) {
            fill_error(err, "minimal.start_failed", "requested start failure");
            return -1;
        }
        if (config_true(inst, "require_dependency")) {
            const void* dep =
                inst->host->dependency(inst->ctx, "dep", kTestInterface);
            if (!dep) {
                fill_error(err, "minimal.dependency_missing",
                           "declared dependency was not injected");
                return -1;
            }
        }
        if (config_true(inst, "wrong_interface_must_be_blocked")) {
            const void* dep =
                inst->host->dependency(inst->ctx, "dep", "wrong.iface");
            if (dep) {
                fill_error(err, "minimal.dependency_leak",
                           "undeclared dependency interface was exposed");
                return -1;
            }
        }
        g_started_count.fetch_add(1, std::memory_order_relaxed);
        return 0;
    };
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        g_shutdown_count.fetch_add(1, std::memory_order_relaxed);
        delete reinterpret_cast<minimal_instance*>(self);
    };
    inst->shell.register_lua = [](struct shield_plugin_instance_v1* self,
                                  struct lua_State*,
                                  struct shield_error_v1* err) {
        auto* inst = reinterpret_cast<minimal_instance*>(self);
        if (config_true(inst, "register_lua_fail")) {
            fill_error(err, "minimal.lua_failed",
                       "requested Lua registration failure", "lua_register");
            return -1;
        }
        return 0;
    };
    *out = &inst->shell;
    return 0;
}

extern "C" SHIELD_PLUGIN_EXPORT int shield_minimal_test_started_count(void) {
    return g_started_count.load(std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT int shield_minimal_test_shutdown_count(void) {
    return g_shutdown_count.load(std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT void shield_minimal_test_reset_counts(void) {
    g_started_count.store(0, std::memory_order_relaxed);
    g_shutdown_count.store(0, std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(struct shield_plugin_abi_v1),
        "minimal.test",
        "1.0.0",
        minimal_create,
    };
    return &abi;
}
