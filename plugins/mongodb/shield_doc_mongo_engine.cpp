// [SHIELD_PLUGIN] database.mongodb — process-wide mongocxx init,
// instance registry, config parsing, pool checkout, the JSON<->BSON
// document builders and the pool-stats vtable.
// Split out of shield_doc_mongodb.cpp.
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

namespace {
// ---------------------------------------------------------------------------
// Process-wide mongocxx initialization.
//
// mongocxx::instance must be constructed exactly once per process before any
// client is created, and must outlive every client. A plugin shutdown does
// NOT release it; this is intentional — Shield's plugin pipeline guarantees
// no other plugin uses mongocxx, and re-initializing across load/unload
// cycles would be a no-op anyway.
// ---------------------------------------------------------------------------
std::once_flag g_mongo_init_flag;
mongocxx::instance* g_mongo_instance = nullptr;
}  // namespace

void ensure_mongo_instance() {
    std::call_once(g_mongo_init_flag, []() {
        // The instance lives until process exit; intentional leak (matches
        // mongocxx documentation: "An instance must remain valid for as
        // long as the application uses the driver.").
        g_mongo_instance = new mongocxx::instance{};
    });
}

namespace {
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}

std::map<std::string, mongo_instance*>& instances_map() {
    static std::map<std::string, mongo_instance*> m;
    return m;
}
}  // namespace

void register_instance(mongo_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}

void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}

mongo_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Config parsing.
// ---------------------------------------------------------------------------
void parse_instance_config(mongo_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("uri") && j["uri"].is_string()) {
            inst->uri_string = j["uri"].get<std::string>();
        }
        if (j.contains("database") && j["database"].is_string()) {
            inst->database_name = j["database"].get<std::string>();
        }
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("socket_timeout_ms") &&
            j["socket_timeout_ms"].is_number_integer()) {
            inst->socket_timeout_ms = j["socket_timeout_ms"].get<int>();
        }
        if (j.contains("pool_size") && j["pool_size"].is_number_integer()) {
            inst->pool_size = j["pool_size"].get<int>();
        }
    } catch (...) {
        // Host validated config against config_schema; tolerate anything
        // malformed by silently falling back to defaults.
    }
}

// Resolve the database name: explicit `database` wins, otherwise extract
// from the URI path (mongocxx::uri::database()).
std::string resolve_database(const mongo_instance* inst) {
    if (!inst->database_name.empty()) return inst->database_name;
    auto db = inst->uri_config.database();
    if (!db.empty()) return std::string(db);
    return "test";  // mongocxx's own default fallback
}

// Try to acquire a client. The pool itself blocks when exhausted; we don't
// add our own timeout here — mongocxx's pool uses the URI's connectTimeoutMS
// as the acquire wait, which the user tuned via `connect_timeout_ms`.
pool_guard acquire_client(mongo_instance* inst, std::string* err) {
    if (!inst || !inst->pool) {
        if (err) *err = "mongodb: pool not initialized";
        return pool_guard{};
    }
    try {
        auto entry = inst->pool->acquire();
        return pool_guard{inst, std::move(entry)};
    } catch (const std::exception& e) {
        if (err)
            *err = std::string("mongodb: pool acquire failed: ") + e.what();
        return pool_guard{};
    }
}

// ---------------------------------------------------------------------------
// JSON helpers.
// ---------------------------------------------------------------------------
// Parse a (possibly NULL) JSON string into a bsoncxx document. Empty/NULL
// becomes an empty document. Throws bsoncxx::exception on parse failure.
bsoncxx::document::value json_to_doc(const char* json) {
    if (!json || !json[0]) return bsoncxx::builder::basic::make_document();
    return bsoncxx::from_json(std::string(json));
}

namespace {
// Parse into an array (for insert_many / aggregate pipeline).
bsoncxx::document::value json_to_array_doc(const char* json) {
    // bsoncxx::from_json handles arrays too; we just need a view.
    if (!json || !json[0]) return bsoncxx::builder::basic::make_document();
    return bsoncxx::from_json(std::string(json));
}
}  // namespace

// Map mongocxx/bsoncxx exceptions to stable error codes. Conservative
// mapping: anything we don't recognize is "db_query_failed" — a catch-all
// that still signals failure to the caller. Connection timeouts surface as
// operation_exception in mongocxx 4.x (the dedicated
// connection_timeout_exception type was removed).
const char* map_exception(const std::exception& e) {
    if (dynamic_cast<const mongocxx::operation_exception*>(&e))
        return "db_query_failed";
    if (dynamic_cast<const mongocxx::logic_error*>(&e)) return "plugin_error";
    if (dynamic_cast<const mongocxx::bulk_write_exception*>(&e))
        return "db_query_failed";
    if (dynamic_cast<const bsoncxx::exception*>(&e))
        return "mapper_unsafe_sql";  // JSON/BSON parse error
    return "db_query_failed";
}

namespace {
// shield.pool.stats.v1 — mongocxx::pool exposes only the URI-configured
// pool bounds (maxPoolSize / minPoolSize query params); live occupancy is
// not observable through the C++ driver, so those gauges stay -1 (unknown).
// max_size mirrors the URI only when explicitly set; the driver default
// (which we don't parse) reads as unknown.
int mongo_pool_get_stats(struct shield_plugin_instance_v1* self,
                         struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<mongo_instance*>(self);
    if (!inst || !out) return -1;
    out->max_size = -1;
    out->size = -1;
    out->idle = -1;
    out->in_use = -1;
    out->waiters = -1;
    out->acquire_timeout_total = -1;
    out->acquire_total = -1;
    out->create_total = -1;
    out->destroy_total = -1;
    out->eviction_total = -1;
    out->health_check_failures_total = -1;
    out->last_error_epoch_ms = -1;
    if (inst->pool) {
        if (auto mx = inst->uri_config.max_pool_size()) {
            if (*mx >= 0 && *mx <= static_cast<int64_t>(
                                       std::numeric_limits<int32_t>::max())) {
                out->max_size = static_cast<int32_t>(*mx);
            }
        }
    }
    return 0;
}
}  // namespace

const shield_pool_stats_v1& mongo_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &mongo_pool_get_stats,
    };
    return v;
}
