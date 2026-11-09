// [SHIELD_PLUGIN] database.mongodb — internal cross-TU declarations for the
// shield_doc_mongodb split.
//
// Not a public contract: the plugin ABI stays in shield/plugin/*.h. The
// data structures below move here verbatim so the shield_doc_mongo_*.cpp
// translation units share one definition; the functions are defined flat
// (external linkage) in their own TU, one declaration each.
#pragma once

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <map>
#include <memory>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/document.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"

// ---------------------------------------------------------------------------
// Instance registry.
// ---------------------------------------------------------------------------
struct mongo_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string uri_string;     // e.g. "mongodb://localhost:27017"
    std::string database_name;  // logical db; may be empty (use URI default)
    int connect_timeout_ms = 5000;
    int socket_timeout_ms = 30000;
    int pool_size = 4;

    std::unique_ptr<mongocxx::pool> pool;
    // Optional URI override for timeouts (set after pool construction).
    mongocxx::uri uri_config;
};

// ---------------------------------------------------------------------------
// Connection pool RAII.
//
// mongocxx::pool::acquire_client returns a pool::entry (move-only). We wrap
// it so callers get a clean RAII type.
// ---------------------------------------------------------------------------
struct pool_guard {
    mongo_instance* inst = nullptr;
    std::unique_ptr<mongocxx::pool::entry> entry;
    bool bad = false;  // true if the caller observed a fatal exception

    pool_guard() = default;
    pool_guard(mongo_instance* i, mongocxx::pool::entry e)
        : inst(i),
          entry(std::make_unique<mongocxx::pool::entry>(std::move(e))) {}
    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&&) noexcept = default;
    pool_guard& operator=(pool_guard&&) noexcept = default;
    ~pool_guard() = default;

    mongocxx::client* operator->() const { return entry ? &**entry : nullptr; }
    mongocxx::client* get() const { return entry ? &**entry : nullptr; }
    explicit operator bool() const { return get() != nullptr; }
};

// Defined in shield_doc_mongo_engine.cpp: process init, registry, config,
// pool checkout, json<->bson builders, exception mapping, pool stats.
void ensure_mongo_instance();
void register_instance(mongo_instance* inst);
void unregister_instance(const std::string& id);
mongo_instance* find_instance(const std::string& id);
void parse_instance_config(mongo_instance* inst, const char* config_json);
std::string resolve_database(const mongo_instance* inst);
pool_guard acquire_client(mongo_instance* inst, std::string* err);
bsoncxx::document::value json_to_doc(const char* json);
const char* map_exception(const std::exception& e);
const shield_pool_stats_v1& mongo_pool_stats_vtable();

// Defined in shield_doc_mongo_exec.cpp: C-ABI executor and vtable glue.
std::string doc_to_json(const bsoncxx::document::view& view);
std::string id_to_json(bsoncxx::types::bson_value::view v);
const shield_document_v1& doc_vtable();

// Defined in shield_doc_mongo_lua.cpp: Lua facade installer.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
