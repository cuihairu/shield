// [SHIELD_PLUGIN] database.mongodb — plugin entry: instance lifecycle
// (mongo_create) and the get_v1 export. Engine, C-ABI executor and
// Lua facade live in the sibling translation units.
//
// Implements the v1 ABI (shield_plugin_get_v1) on top of mongo-cxx-driver.
// The plugin owns its own mongocxx::pool — one per instance — sized by the
// `pool_size` config knob. Lua callers reach the pool via the callable
// namespace `shield.database.mongodb(binding)`.
//
// JSON over the C ABI: every filter / doc / update / pipeline is a UTF-8
// JSON string. We parse it to nlohmann::json first for tolerant validation,
// then rebuild as bsoncxx::document::value. MongoDB-specific JSON
// extensions ({"$oid": ...}, {"$date": ...}, {"$gt": ...}, ...) flow
// through unchanged: mongocxx's from_json understands them.

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/logic_error.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/document.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield_doc_mongo_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

// The C ABI connection handle. Defined at global scope to complete the
// opaque struct forward-declared in shield/plugin/document.h — the vtable
// below assigns these functions to ABI-declared function pointers, so both
// must name the same type.

// ---------------------------------------------------------------------------
// v1 ABI: create / shutdown.
// ---------------------------------------------------------------------------
int mongo_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    ensure_mongo_instance();

    auto* inst = new mongo_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    if (inst->uri_string.empty()) {
        inst->uri_string = "mongodb://localhost:27017";
    }
    try {
        inst->uri_config = mongocxx::uri(inst->uri_string);
        inst->pool = std::make_unique<mongocxx::pool>(inst->uri_config);
    } catch (const std::exception& e) {
        delete inst;
        if (err) {
            err->code = "plugin.create.failed";
            err->message = "database.mongodb: invalid uri or pool init";
        }
        (void)e;
        return 1;
    }
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_DOCUMENT_INTERFACE) == 0)
            return &doc_vtable();
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0)
            return &mongo_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* inst = reinterpret_cast<mongo_instance*>(self);
        unregister_instance(inst->instance_id);
        // pool unique_ptr releases all clients on destruction.
        inst->pool.reset();
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.mongodb",
        "1.0.0",
        mongo_create,
    };
    return &abi;
}
