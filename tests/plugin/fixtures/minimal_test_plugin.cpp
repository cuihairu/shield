// Minimal test plugin used by test_plugin_host / test_plugin_e2e to verify the
// load/create/start pipeline end-to-end without depending on sqlite/mysql.
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

#include <atomic>
#include <cstring>
#include <string>

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

void fill_error(shield_error_v1* err,
                const char* code,
                const char* message,
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
    inst->shell.get_interface = [](struct shield_plugin_instance_v1*,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, kTestInterface) == 0) {
            return &test_vtable();
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
            const void* dep = inst->host->dependency(inst->ctx, "dep", kTestInterface);
            if (!dep) {
                fill_error(err, "minimal.dependency_missing",
                           "declared dependency was not injected");
                return -1;
            }
        }
        if (config_true(inst, "wrong_interface_must_be_blocked")) {
            const void* dep = inst->host->dependency(inst->ctx, "dep", "wrong.iface");
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

extern "C" SHIELD_PLUGIN_EXPORT
int shield_minimal_test_started_count(void) {
    return g_started_count.load(std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT
int shield_minimal_test_shutdown_count(void) {
    return g_shutdown_count.load(std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT
void shield_minimal_test_reset_counts(void) {
    g_started_count.store(0, std::memory_order_relaxed);
    g_shutdown_count.store(0, std::memory_order_relaxed);
}

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(struct shield_plugin_abi_v1),
        "minimal.test",
        "1.0.0",
        minimal_create,
    };
    return &abi;
}
