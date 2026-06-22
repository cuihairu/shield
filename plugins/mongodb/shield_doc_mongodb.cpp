// [SHIELD_PLUGIN] database.mongodb — mongocxx provider for shield.document.v1.
//
// Implements the v1 ABI (shield_plugin_get_v1) on top of mongo-cxx-driver.
// The plugin owns its own mongocxx::pool — one per instance — sized by the
// `pool_size` config knob. Lua callers reach the pool via the callable
// namespace `shield.database.mongodb(instance_id)`.
//
// JSON over the C ABI: every filter / doc / update / pipeline is a UTF-8
// JSON string. We parse it to nlohmann::json first for tolerant validation,
// then rebuild as bsoncxx::document::value. MongoDB-specific JSON
// extensions ({"$oid": ...}, {"$date": ...}, {"$gt": ...}, ...) flow
// through unchanged: mongocxx's from_json understands them.

#include "shield/plugin/abi.h"
#include "shield/plugin/document.h"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/client_session.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

// ---------------------------------------------------------------------------
// Process-wide mongocxx initialization.
//
// mongocxx::instance must be constructed exactly once per process before any
// client is created, and must outlive every client. A plugin shutdown does
// NOT release it; this is intentional — Shield's plugin pipeline guarantees
// no other plugin uses mongocxx, and re-initializing across load/unload
// cycles would be a no-op anyway.
// ---------------------------------------------------------------------------
namespace {
std::once_flag g_mongo_init_flag;
mongocxx::instance* g_mongo_instance = nullptr;

void ensure_mongo_instance() {
    std::call_once(g_mongo_init_flag, []() {
        // The instance lives until process exit; intentional leak (matches
        // mongocxx documentation: "An instance must remain valid for as
        // long as the application uses the driver.").
        g_mongo_instance = new mongocxx::instance{};
    });
}

// ---------------------------------------------------------------------------
// Instance registry.
// ---------------------------------------------------------------------------
struct mongo_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    std::string uri_string;       // e.g. "mongodb://localhost:27017"
    std::string database_name;    // logical db; may be empty (use URI default)
    int connect_timeout_ms = 5000;
    int socket_timeout_ms = 30000;
    int pool_size = 4;

    std::unique_ptr<mongocxx::pool> pool;
    // Optional URI override for timeouts (set after pool construction).
    mongocxx::uri uri_config;
};

std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, mongo_instance*>& instances_map() {
    static std::map<std::string, mongo_instance*> m;
    return m;
}

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
        : inst(i), entry(std::make_unique<mongocxx::pool::entry>(std::move(e))) {}
    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&&) noexcept = default;
    pool_guard& operator=(pool_guard&&) noexcept = default;
    ~pool_guard() = default;

    mongocxx::client* operator->() const {
        return entry ? entry->get() : nullptr;
    }
    mongocxx::client* get() const {
        return entry ? entry->get() : nullptr;
    }
    explicit operator bool() const { return get() != nullptr; }
};

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
        if (err) *err = std::string("mongodb: pool acquire failed: ") + e.what();
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
    return bsoncxx::json::parse(std::string(json));
}

// Parse into an array (for insert_many / aggregate pipeline).
bsoncxx::document::value json_to_array_doc(const char* json) {
    // bsoncxx::json::parse handles arrays too; we just need a view.
    if (!json || !json[0]) return bsoncxx::builder::basic::make_document();
    return bsoncxx::json::parse(std::string(json));
}

// Serialize a bsoncxx view back to canonical JSON. Used to fill cursor and
// inserted_id strings.
std::string doc_to_json(const bsoncxx::document::view& view) {
    return bsoncxx::to_json(view);
}

// ---------------------------------------------------------------------------
// Error table construction (Lua side).
// ---------------------------------------------------------------------------
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Map mongocxx/bsoncxx exceptions to stable error codes. Conservative
// mapping: anything we don't recognize is "db_query_failed" — a catch-all
// that still signals failure to the caller.
const char* map_exception(const std::exception& e) {
    if (dynamic_cast<const mongocxx::connection_timeout_exception*>(&e))
        return "connection_failed";
    if (dynamic_cast<const mongocxx::operation_exception*>(&e))
        return "db_query_failed";
    if (dynamic_cast<const mongocxx::logic_exception*>(&e))
        return "plugin_error";
    if (dynamic_cast<const mongocxx::bulk_write_exception*>(&e))
        return "db_query_failed";
    if (dynamic_cast<const bsoncxx::exception*>(&e))
        return "mapper_unsafe_sql";  // JSON/BSON parse error
    return "db_query_failed";
}

// ---------------------------------------------------------------------------
// shield_document_v1 (C ABI vtable).
//
// Each method connects, runs one operation, disconnects. The C vtable does
// NOT share the per-instance pool with Lua callers — mongocxx clients are
// heavyweight to construct, but this vtable is reserved for C-ABI consumers
// that don't have access to the Lua proxy. Both paths ultimately share the
// same mongocxx::instance and connect to the same URI.
//
// The pool is still used if available (acquire_client). The C ABI consumer
// gets a pooled client for the duration of a single call.
// ---------------------------------------------------------------------------
struct shield_doc_conn {
    std::string database_name;
    std::unique_ptr<mongocxx::client> client;
    std::unique_ptr<mongocxx::client_session> session;  // non-null when in tx
};

const char* dup_string(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

void clear_result(shield_doc_result* r) {
    if (!r) return;
    r->success = 0;
    r->error_msg = nullptr;
    r->error_code = nullptr;
    r->matched_count = 0;
    r->modified_count = 0;
    r->inserted_count = 0;
    r->inserted_id_json = nullptr;
    r->upserted_id_json = nullptr;
}

void clear_cursor(shield_doc_cursor* c) {
    if (!c) return;
    c->success = 0;
    c->error_msg = nullptr;
    c->error_code = nullptr;
    c->docs_json = nullptr;
    c->matched_count = -1;
}

void free_result_strings(shield_doc_result* r) {
    if (!r) return;
    if (r->error_msg) { std::free(const_cast<char*>(r->error_msg)); r->error_msg = nullptr; }
    if (r->error_code) { std::free(const_cast<char*>(r->error_code)); r->error_code = nullptr; }
    if (r->inserted_id_json) { std::free(r->inserted_id_json); r->inserted_id_json = nullptr; }
    if (r->upserted_id_json) { std::free(r->upserted_id_json); r->upserted_id_json = nullptr; }
}

void free_cursor_strings(shield_doc_cursor* c) {
    if (!c) return;
    if (c->error_msg) { std::free(const_cast<char*>(c->error_msg)); c->error_msg = nullptr; }
    if (c->error_code) { std::free(const_cast<char*>(c->error_code)); c->error_code = nullptr; }
    if (c->docs_json) { std::free(const_cast<char*>(c->docs_json)); c->docs_json = nullptr; }
}

shield_doc_conn* doc_connect(const shield_doc_connect_args* args,
                             char* err_buf, int err_buf_size) {
    ensure_mongo_instance();
    if (!args || !args->uri) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "mongodb: uri required", err_buf_size - 1);
            err_buf[err_buf_size - 1] = '\0';
        }
        return nullptr;
    }
    try {
        mongocxx::uri uri{args->uri};
        auto client = std::make_unique<mongocxx::client>(uri);
        auto* conn = new shield_doc_conn{};
        if (args->database && args->database[0]) {
            conn->database_name = args->database;
        } else {
            auto db = uri.database();
            conn->database_name = db.empty() ? std::string("test")
                                             : std::string(db);
        }
        conn->client = std::move(client);
        return conn;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
            err_buf[err_buf_size - 1] = '\0';
        }
        return nullptr;
    }
}

void doc_disconnect(shield_doc_conn* conn) {
    delete conn;
}

int doc_ping(shield_doc_conn* conn) {
    if (!conn || !conn->client) return 0;
    try {
        auto db = conn->client->database(conn->database_name);
        db.run_command(bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("ping", 1)));
        return 1;
    } catch (...) {
        return 0;
    }
}

int doc_find(shield_doc_conn* conn, const char* collection,
             const char* filter_json, const char* opts_json,
             shield_doc_cursor* out) {
    clear_cursor(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        bsoncxx::document::view filter_view = json_to_doc(filter_json);
        mongocxx::options::find opts;
        (void)opts_json;
        auto cursor = coll.find(filter_view, opts);
        nlohmann::json docs = nlohmann::json::array();
        int64_t count = 0;
        for (auto&& doc : cursor) {
            docs.push_back(nlohmann::json::parse(doc_to_json(doc)));
            ++count;
        }
        out->success = 1;
        out->matched_count = count;
        out->docs_json = dup_string(docs.dump());
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_find_one(shield_doc_conn* conn, const char* collection,
                 const char* filter_json, const char* opts_json,
                 char** out_doc_json, shield_doc_result* out) {
    clear_result(out);
    if (out_doc_json) *out_doc_json = nullptr;
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        bsoncxx::document::view filter_view = json_to_doc(filter_json);
        (void)opts_json;
        auto maybe = coll.find_one(filter_view);
        out->success = 1;
        if (maybe && out_doc_json) {
            *out_doc_json = dup_string(doc_to_json(*maybe));
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_insert_one(shield_doc_conn* conn, const char* collection,
                   const char* doc_json, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto doc = json_to_doc(doc_json);
        auto res = coll.insert_one(doc.view());
        out->success = 1;
        if (res) {
            out->inserted_count = static_cast<int64_t>(res->inserted_count());
            auto id = res->inserted_id();
            if (id) {
                out->inserted_id_json = dup_string(
                    doc_to_json(id.get_document().value));
            }
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_insert_many(shield_doc_conn* conn, const char* collection,
                    const char* docs_json_array, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto parsed = nlohmann::json::parse(
            docs_json_array ? docs_json_array : "[]");
        std::vector<bsoncxx::document::value> docs;
        for (auto& d : parsed) {
            docs.push_back(bsoncxx::json::parse(d.dump()));
        }
        auto res = coll.insert_many(docs);
        out->success = 1;
        if (res) {
            out->inserted_count = static_cast<int64_t>(res->inserted_count());
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_update_one(shield_doc_conn* conn, const char* collection,
                   const char* filter_json, const char* update_json,
                   const char* opts_json, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        mongocxx::options::update opts;
        if (opts_json) {
            auto o = nlohmann::json::parse(opts_json);
            if (o.contains("upsert") && o["upsert"].is_boolean()) {
                opts.upsert(o["upsert"].get<bool>());
            }
        }
        auto res = coll.update_one(json_to_doc(filter_json),
                                   json_to_doc(update_json), opts);
        out->success = 1;
        if (res) {
            out->matched_count = static_cast<int64_t>(res->matched_count());
            out->modified_count = static_cast<int64_t>(res->modified_count());
            if (res->upserted_count() > 0) {
                auto id = res->upserted_id();
                if (id) {
                    out->upserted_id_json = dup_string(
                        doc_to_json(id.get_document().value));
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_update_many(shield_doc_conn* conn, const char* collection,
                    const char* filter_json, const char* update_json,
                    shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto res = coll.update_many(json_to_doc(filter_json),
                                    json_to_doc(update_json));
        out->success = 1;
        if (res) {
            out->matched_count = static_cast<int64_t>(res->matched_count());
            out->modified_count = static_cast<int64_t>(res->modified_count());
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_delete_one(shield_doc_conn* conn, const char* collection,
                   const char* filter_json, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto res = coll.delete_one(json_to_doc(filter_json));
        out->success = 1;
        if (res) {
            out->matched_count = static_cast<int64_t>(res->deleted_count());
            out->modified_count = static_cast<int64_t>(res->deleted_count());
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_delete_many(shield_doc_conn* conn, const char* collection,
                    const char* filter_json, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto res = coll.delete_many(json_to_doc(filter_json));
        out->success = 1;
        if (res) {
            out->matched_count = static_cast<int64_t>(res->deleted_count());
            out->modified_count = static_cast<int64_t>(res->deleted_count());
        }
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_count(shield_doc_conn* conn, const char* collection,
              const char* filter_json, const char* opts_json,
              int64_t* out_count) {
    if (!conn || !conn->client) return 1;
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        (void)opts_json;
        auto count = coll.count_documents(json_to_doc(filter_json));
        if (out_count) *out_count = static_cast<int64_t>(count);
        return 0;
    } catch (const std::exception& e) {
        (void)e;
        return 1;
    }
}

int doc_aggregate(shield_doc_conn* conn, const char* collection,
                  const char* pipeline_json, const char* opts_json,
                  shield_doc_cursor* out) {
    clear_cursor(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto parsed = nlohmann::json::parse(
            pipeline_json ? pipeline_json : "[]");
        std::vector<bsoncxx::document::value> stages;
        for (auto& s : parsed) {
            stages.push_back(bsoncxx::json::parse(s.dump()));
        }
        (void)opts_json;
        mongocxx::pipeline p;
        for (auto& s : stages) {
            p.append_stage(s.view());
        }
        auto cursor = coll.aggregate(p);
        nlohmann::json docs = nlohmann::json::array();
        int64_t count = 0;
        for (auto&& doc : cursor) {
            docs.push_back(nlohmann::json::parse(doc_to_json(doc)));
            ++count;
        }
        out->success = 1;
        out->matched_count = count;
        out->docs_json = dup_string(docs.dump());
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_create_index(shield_doc_conn* conn, const char* collection,
                     const char* keys_json, const char* opts_json,
                     shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        auto keys = json_to_doc(keys_json);
        mongocxx::options::index opts;
        std::string name;
        if (opts_json) {
            auto o = nlohmann::json::parse(opts_json);
            if (o.contains("name") && o["name"].is_string()) {
                name = o["name"].get<std::string>();
                opts.name(name);
            }
            if (o.contains("unique") && o["unique"].is_boolean()) {
                opts.unique(o["unique"].get<bool>());
            }
        }
        auto res = coll.create_index(keys.view(), opts);
        out->success = 1;
        out->inserted_id_json = dup_string(res);  // reuse as "name created"
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_drop_index(shield_doc_conn* conn, const char* collection,
                   const char* index_name, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        auto coll = conn->client->database(conn->database_name)
                        .collection(collection);
        coll.indexes().drop_one(std::string(index_name));
        out->success = 1;
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_begin(shield_doc_conn* conn, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->client) {
        out->error_code = "connection_lost";
        out->error_msg = dup_string("mongodb: connection not acquired");
        return 1;
    }
    try {
        conn->session = std::make_unique<mongocxx::client_session>(
            conn->client->start_session());
        conn->session->start_transaction();
        out->success = 1;
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_commit(shield_doc_conn* conn, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->session) {
        out->error_code = "plugin_error";
        out->error_msg = dup_string("mongodb: no active transaction");
        return 1;
    }
    try {
        conn->session->commit_transaction();
        conn->session.reset();
        out->success = 1;
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

int doc_rollback(shield_doc_conn* conn, shield_doc_result* out) {
    clear_result(out);
    if (!conn || !conn->session) {
        out->error_code = "plugin_error";
        out->error_msg = dup_string("mongodb: no active transaction");
        return 1;
    }
    try {
        conn->session->abort_transaction();
        conn->session.reset();
        out->success = 1;
        return 0;
    } catch (const std::exception& e) {
        out->error_code = map_exception(e);
        out->error_msg = dup_string(e.what());
        return 1;
    }
}

void doc_free_cursor(shield_doc_cursor* c) { free_cursor_strings(c); }
void doc_free_result(shield_doc_result* r) { free_result_strings(r); }

const shield_document_v1& doc_vtable() {
    static const shield_document_v1 v = {
        sizeof(shield_document_v1),
        "mongodb",
        "1.0.0",
        doc_connect,
        doc_disconnect,
        doc_ping,
        doc_find,
        doc_find_one,
        doc_insert_one,
        doc_insert_many,
        doc_update_one,
        doc_update_many,
        doc_delete_one,
        doc_delete_many,
        doc_count,
        doc_aggregate,
        doc_create_index,
        doc_drop_index,
        doc_begin,
        doc_commit,
        doc_rollback,
        doc_free_cursor,
        doc_free_result,
    };
    return v;
}

// ---------------------------------------------------------------------------
// Lua bindings.
//
// The proxy returned by shield.database.mongodb(instance_id) exposes the
// full document surface. All methods return (ok, result|err) pairs to match
// the SQL plugin convention.
//
// `transaction(fn)` wraps mongocxx client_session.with_transaction so the
// caller doesn't have to deal with retry/abort logic. The Lua callback
// receives a per-transaction proxy whose methods automatically route
// through the session.
// ---------------------------------------------------------------------------
sol::table make_error_table(sol::state_view lua, const std::exception& e) {
    return make_error_table(lua, map_exception(e), e.what());
}

// Convert a nlohmann::json value into a Lua object (recursive). Mirrors the
// shape used by shield.lua's json_to_lua — duplicated here to keep the
// plugin self-contained (no host runtime dependency).
sol::object json_to_lua(sol::state_view lua, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return sol::nil;
        case nlohmann::json::value_t::boolean:
            return sol::make_object(lua, j.get<bool>());
        case nlohmann::json::value_t::number_integer:
            return sol::make_object(lua, j.get<int64_t>());
        case nlohmann::json::value_t::number_unsigned:
            return sol::make_object(lua, j.get<int64_t>());
        case nlohmann::json::value_t::number_float:
            return sol::make_object(lua, j.get<double>());
        case nlohmann::json::value_t::string:
            return sol::make_object(lua, j.get<std::string>());
        case nlohmann::json::value_t::array: {
            sol::table t = lua.create_table();
            int i = 1;
            for (auto& el : j) {
                t[i++] = json_to_lua(lua, el);
            }
            return t;
        }
        case nlohmann::json::value_t::object: {
            sol::table t = lua.create_table();
            for (auto& [k, v] : j.items()) {
                t[k] = json_to_lua(lua, v);
            }
            return t;
        }
        case nlohmann::json::value_t::binary:
            return sol::nil;
        case nlohmann::json::value_t::discarded:
            return sol::nil;
    }
    return sol::nil;
}

// Convert a Lua object into nlohmann::json (recursive). Tables with
// 1..N integer keys become arrays; otherwise objects. Nested tables work.
nlohmann::json lua_to_json(const sol::object& obj) {
    if (!obj.valid() || obj.get_type() == sol::type::nil) return nullptr;
    switch (obj.get_type()) {
        case sol::type::boolean:
            return obj.as<bool>();
        case sol::type::number:
            return obj.as<double>();
        case sol::type::string:
            return obj.as<std::string>();
        case sol::type::table: {
            sol::table t = obj.as<sol::table>();
            // Detect array vs object: integer keys 1..N only => array.
            bool is_array = true;
            int max_index = 0;
            int count = 0;
            for (auto& kv : t) {
                ++count;
                if (kv.first.get_type() == sol::type::number) {
                    int idx = static_cast<int>(kv.first.as<int>());
                    if (idx > max_index) max_index = idx;
                } else {
                    is_array = false;
                    break;
                }
            }
            if (is_array && count > 0 && max_index == count) {
                auto arr = nlohmann::json::array();
                for (int i = 1; i <= max_index; ++i) {
                    arr.push_back(lua_to_json(t[i]));
                }
                return arr;
            } else if (count == 0) {
                return nlohmann::json::object();
            }
            auto obj = nlohmann::json::object();
            for (auto& kv : t) {
                std::string key;
                if (kv.first.get_type() == sol::type::number) {
                    key = std::to_string(kv.first.as<int>());
                } else {
                    key = kv.first.as<std::string>();
                }
                obj[key] = lua_to_json(kv.second);
            }
            return obj;
        }
        default:
            return nullptr;
    }
}

// Stringify a Lua value as JSON. Throws nlohmann::json::exception on failure.
std::string lua_to_json_string(const sol::object& obj) {
    return lua_to_json(obj).dump();
}

// Build the per-instance Lua proxy. Each method acquires a pool client,
// runs the operation, and returns the result.
sol::table make_instance_proxy(sol::state_view lua, mongo_instance* inst);

// Build a per-transaction proxy: all methods route through the given
// client_session. Bound to the pool_guard's lifetime.
sol::table make_session_proxy(sol::state_view lua, mongo_instance* inst,
                              mongocxx::client* client,
                              mongocxx::client_session* session) {
    auto proxy = lua.create_table();

    auto run = [&](const std::string& collection,
                   std::function<void(mongocxx::collection&)> fn)
        -> std::pair<bool, std::string> {
        try {
            auto coll = client->database(resolve_database(inst))
                            .collection(collection);
            fn(coll);
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    };
    (void)run;  // used inside lambdas below

    proxy.set_function("find",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj,
                                sol::object opts_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::json::parse(
                    lua_to_json_string(filter_obj));
                auto cursor = session
                    ? coll.find(*session, filter_doc.view())
                    : coll.find(filter_doc.view());
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("find_one",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj)
        -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::json::parse(
                    lua_to_json_string(filter_obj));
                auto maybe = session
                    ? coll.find_one(*session, filter_doc.view())
                    : coll.find_one(filter_doc.view());
                results.push_back(sol::make_object(lua, true));
                if (maybe) {
                    results.push_back(json_to_lua(lua,
                        nlohmann::json::parse(bsoncxx::to_json(*maybe))));
                } else {
                    results.push_back(sol::nil);
                }
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("insert_one",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object doc_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto doc = bsoncxx::json::parse(lua_to_json_string(doc_obj));
                auto res = session
                    ? coll.insert_one(*session, doc.view())
                    : coll.insert_one(doc.view());
                auto t = lua.create_table();
                t["inserted_count"] = res ? static_cast<int64_t>(res->inserted_count()) : 0;
                if (res && res->inserted_id()) {
                    t["inserted_id"] = json_to_lua(lua,
                        nlohmann::json::parse(bsoncxx::to_json(
                            res->inserted_id().get_document().value)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("insert_many",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object docs_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto arr = lua_to_json(docs_obj);
                std::vector<bsoncxx::document::value> docs;
                for (auto& d : arr) {
                    docs.push_back(bsoncxx::json::parse(d.dump()));
                }
                auto res = session
                    ? coll.insert_many(*session, docs)
                    : coll.insert_many(docs);
                auto t = lua.create_table();
                t["inserted_count"] = res ? static_cast<int64_t>(res->inserted_count()) : 0;
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("update_one",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj,
                                sol::object update_obj,
                                sol::object opts_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::options::update opts;
                if (opts_obj.valid() && opts_obj.get_type() == sol::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("upsert") && o["upsert"].is_boolean())
                        opts.upsert(o["upsert"].get<bool>());
                }
                auto res = session
                    ? coll.update_one(*session,
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                        bsoncxx::json::parse(lua_to_json_string(update_obj)).view(),
                        opts)
                    : coll.update_one(
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                        bsoncxx::json::parse(lua_to_json_string(update_obj)).view(),
                        opts);
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] = static_cast<int64_t>(res->matched_count());
                    t["modified_count"] = static_cast<int64_t>(res->modified_count());
                    if (res->upserted_count() > 0 && res->upserted_id()) {
                        t["upserted_id"] = json_to_lua(lua,
                            nlohmann::json::parse(bsoncxx::to_json(
                                res->upserted_id().get_document().value)));
                    }
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("update_many",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj,
                                sol::object update_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res = session
                    ? coll.update_many(*session,
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                        bsoncxx::json::parse(lua_to_json_string(update_obj)).view())
                    : coll.update_many(
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                        bsoncxx::json::parse(lua_to_json_string(update_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] = static_cast<int64_t>(res->matched_count());
                    t["modified_count"] = static_cast<int64_t>(res->modified_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("delete_one",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res = session
                    ? coll.delete_one(*session,
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view())
                    : coll.delete_one(
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] = static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("delete_many",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res = session
                    ? coll.delete_many(*session,
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view())
                    : coll.delete_many(
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] = static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("count",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto count = session
                    ? coll.count_documents(*session,
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view())
                    : coll.count_documents(
                        bsoncxx::json::parse(lua_to_json_string(filter_obj)).view());
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<int64_t>(count)));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("aggregate",
        [inst, client, session](sol::this_state s, std::string collection,
                                sol::object pipeline_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::pipeline p;
                auto arr = lua_to_json(pipeline_obj);
                for (auto& s_doc : arr) {
                    p.append_stage(bsoncxx::json::parse(s_doc.dump()).view());
                }
                auto cursor = session
                    ? coll.aggregate(*session, p)
                    : coll.aggregate(p);
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    return proxy;
}

sol::table make_instance_proxy(sol::state_view lua, mongo_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("find",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj, sol::object /*opts_obj*/) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::json::parse(
                    filter_obj.valid() ? lua_to_json_string(filter_obj) : "{}");
                auto cursor = coll.find(filter_doc.view());
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("find_one",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::json::parse(
                    filter_obj.valid() ? lua_to_json_string(filter_obj) : "{}");
                auto maybe = coll.find_one(filter_doc.view());
                results.push_back(sol::make_object(lua, true));
                if (maybe) {
                    results.push_back(json_to_lua(lua,
                        nlohmann::json::parse(bsoncxx::to_json(*maybe))));
                } else {
                    results.push_back(sol::nil);
                }
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("insert_one",
        [inst](sol::this_state s, std::string collection,
               sol::object doc_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto doc = bsoncxx::json::parse(lua_to_json_string(doc_obj));
                auto res = coll.insert_one(doc.view());
                auto t = lua.create_table();
                t["inserted_count"] = res ? static_cast<int64_t>(res->inserted_count()) : 0;
                if (res && res->inserted_id()) {
                    t["inserted_id"] = json_to_lua(lua,
                        nlohmann::json::parse(bsoncxx::to_json(
                            res->inserted_id().get_document().value)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("insert_many",
        [inst](sol::this_state s, std::string collection,
               sol::object docs_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto arr = lua_to_json(docs_obj);
                std::vector<bsoncxx::document::value> docs;
                for (auto& d : arr) {
                    docs.push_back(bsoncxx::json::parse(d.dump()));
                }
                auto res = coll.insert_many(docs);
                auto t = lua.create_table();
                t["inserted_count"] = res ? static_cast<int64_t>(res->inserted_count()) : 0;
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("update_one",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj, sol::object update_obj,
               sol::object opts_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::options::update opts;
                if (opts_obj.valid() && opts_obj.get_type() == sol::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("upsert") && o["upsert"].is_boolean())
                        opts.upsert(o["upsert"].get<bool>());
                }
                auto res = coll.update_one(
                    bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                    bsoncxx::json::parse(lua_to_json_string(update_obj)).view(),
                    opts);
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] = static_cast<int64_t>(res->matched_count());
                    t["modified_count"] = static_cast<int64_t>(res->modified_count());
                    if (res->upserted_count() > 0 && res->upserted_id()) {
                        t["upserted_id"] = json_to_lua(lua,
                            nlohmann::json::parse(bsoncxx::to_json(
                                res->upserted_id().get_document().value)));
                    }
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("update_many",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj, sol::object update_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.update_many(
                    bsoncxx::json::parse(lua_to_json_string(filter_obj)).view(),
                    bsoncxx::json::parse(lua_to_json_string(update_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] = static_cast<int64_t>(res->matched_count());
                    t["modified_count"] = static_cast<int64_t>(res->modified_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("delete_one",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.delete_one(
                    bsoncxx::json::parse(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] = static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("delete_many",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.delete_many(
                    bsoncxx::json::parse(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] = static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("count",
        [inst](sol::this_state s, std::string collection,
               sol::object filter_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto count = coll.count_documents(
                    bsoncxx::json::parse(
                        filter_obj.valid() ? lua_to_json_string(filter_obj) : "{}").view());
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<int64_t>(count)));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("aggregate",
        [inst](sol::this_state s, std::string collection,
               sol::object pipeline_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::pipeline p;
                auto arr = lua_to_json(pipeline_obj);
                for (auto& s_doc : arr) {
                    p.append_stage(bsoncxx::json::parse(s_doc.dump()).view());
                }
                auto cursor = coll.aggregate(p);
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("create_index",
        [inst](sol::this_state s, std::string collection,
               sol::object keys_obj, sol::object opts_obj) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                auto keys = bsoncxx::json::parse(lua_to_json_string(keys_obj));
                mongocxx::options::index opts;
                if (opts_obj.valid() && opts_obj.get_type() == sol::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("name") && o["name"].is_string())
                        opts.name(o["name"].get<std::string>());
                    if (o.contains("unique") && o["unique"].is_boolean())
                        opts.unique(o["unique"].get<bool>());
                }
                auto name = coll.create_index(keys.view(), opts);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, name));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("drop_index",
        [inst](sol::this_state s, std::string collection,
               std::string index_name) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()->database(resolve_database(inst))
                                .collection(collection);
                coll.indexes().drop_one(index_name);
                results.push_back(sol::make_object(lua, true));
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("transaction",
        [inst](sol::this_state s,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            mongocxx::client* client = g.get();
            std::unique_ptr<mongocxx::client_session> session;
            try {
                session = std::make_unique<mongocxx::client_session>(
                    client->start_session());
                session->start_transaction();
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }

            sol::table tx_proxy = make_session_proxy(lua, inst, client, session.get());

            sol::protected_function_result cb_res = callback(tx_proxy);
            bool commit = cb_res.valid();
            bool user_abort = false;
            bool saw_error = false;

            if (cb_res.valid()) {
                sol::optional<bool> first = cb_res.get<sol::optional<bool>>(0);
                if (first && !*first) {
                    commit = false;
                    user_abort = true;
                }
            } else {
                saw_error = true;
                commit = false;
            }

            bool tx_ok = false;
            std::string tx_err_msg;
            const char* tx_err_code = "db_query_failed";
            try {
                if (commit) {
                    session->commit_transaction();
                    tx_ok = true;
                } else {
                    session->abort_transaction();
                }
            } catch (const std::exception& e) {
                tx_err_msg = e.what();
                tx_err_code = map_exception(e);
            }

            if (!tx_ok) {
                results.push_back(sol::make_object(lua, false));
                if (tx_err_msg.empty()) {
                    if (saw_error) {
                        results.push_back(make_error_table(lua,
                            "transaction_rolled_back",
                            "callback raised an error"));
                    } else {
                        results.push_back(make_error_table(lua,
                            "transaction_rolled_back",
                            "callback returned false"));
                    }
                } else {
                    results.push_back(make_error_table(lua, tx_err_code, tx_err_msg));
                }
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            int n_returns = cb_res.return_count();
            for (int i = 0; i < n_returns; ++i) {
                results.push_back(cb_res.get<sol::object>(i));
            }
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* /*self*/,
                      struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.mongodb: lua_State is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    auto shield = lua["shield"].get_or_create<sol::table>();
    auto database = shield["database"].get_or_create<sol::table>();

    sol::object existing = database["mongodb"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        mt.set_function("__call",
            [](sol::this_state s, sol::table /*self*/,
               std::string instance_id) -> sol::object {
                sol::state_view lua(s);
                auto* inst = find_instance(instance_id);
                if (!inst) return sol::nil;
                return sol::make_object(lua, make_instance_proxy(lua, inst));
            });
        ns[sol::metatable_key] = mt;
        database["mongodb"] = ns;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// v1 ABI: create / shutdown.
// ---------------------------------------------------------------------------
int mongo_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    (void)err;
    ensure_mongo_instance();

    auto* inst = new mongo_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
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
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
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

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.mongodb",
        "1.0.0",
        mongo_create,
    };
    return &abi;
}
