// Minimal test plugin used by test_plugin_host / test_plugin_e2e to verify the
// load/create/start pipeline end-to-end without depending on sqlite/mysql.
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

static int minimal_create(const struct shield_plugin_create_args_v1* args,
                          struct shield_plugin_instance_v1** out,
                          struct shield_error_v1* err) {
    (void)args; (void)err;
    static struct shield_plugin_instance_v1 inst;
    inst.struct_size = sizeof(inst);
    inst.instance_id = "minimal";
    inst.get_interface = [](struct shield_plugin_instance_v1*, const char*,
                            struct shield_error_v1*) -> const void* { return nullptr; };
    inst.start = [](struct shield_plugin_instance_v1*, struct shield_error_v1*) { return 0; };
    inst.shutdown = [](struct shield_plugin_instance_v1*) {};
    *out = &inst;
    return 0;
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
