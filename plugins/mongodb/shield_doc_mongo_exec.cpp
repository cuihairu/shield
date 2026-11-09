// [SHIELD_PLUGIN] database.mongodb — C-ABI executor: the connect/find/
// insert/update/delete/count/aggregate/index/transaction operations,
// result/cursor string helpers and the document vtable glue.
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

struct shield_doc_conn {
    std::string database_name;
    std::unique_ptr<mongocxx::client> client;
    std::unique_ptr<mongocxx::client_session> session;  // non-null when in tx
};

// Serialize a bsoncxx view back to canonical JSON. Used to fill cursor and
// inserted_id strings.
std::string doc_to_json(const bsoncxx::document::view& view) {
    return bsoncxx::to_json(view);
}

// Serialize a single BSON value (the _id handed back by insert/update
// results) as canonical JSON. mongocxx 4.x returns these as typed values
// rather than embedded documents, so wrap one in a document for to_json.
std::string id_to_json(bsoncxx::types::bson_value::view v) {
    using bsoncxx::builder::basic::kvp;
    return bsoncxx::to_json(
        bsoncxx::builder::basic::make_document(kvp("_id", v)).view());
}

namespace {
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
char* dup_string(const std::string& s) {
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
    if (r->error_msg) {
        std::free(const_cast<char*>(r->error_msg));
        r->error_msg = nullptr;
    }
    if (r->error_code) {
        std::free(const_cast<char*>(r->error_code));
        r->error_code = nullptr;
    }
    if (r->inserted_id_json) {
        std::free(r->inserted_id_json);
        r->inserted_id_json = nullptr;
    }
    if (r->upserted_id_json) {
        std::free(r->upserted_id_json);
        r->upserted_id_json = nullptr;
    }
}

void free_cursor_strings(shield_doc_cursor* c) {
    if (!c) return;
    if (c->error_msg) {
        std::free(const_cast<char*>(c->error_msg));
        c->error_msg = nullptr;
    }
    if (c->error_code) {
        std::free(const_cast<char*>(c->error_code));
        c->error_code = nullptr;
    }
    if (c->docs_json) {
        std::free(const_cast<char*>(c->docs_json));
        c->docs_json = nullptr;
    }
}

shield_doc_conn* doc_connect(const shield_doc_connect_args* args, char* err_buf,
                             int err_buf_size) {
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
            conn->database_name =
                db.empty() ? std::string("test") : std::string(db);
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

void doc_disconnect(shield_doc_conn* conn) { delete conn; }

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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
        auto doc = json_to_doc(doc_json);
        auto res = coll.insert_one(doc.view());
        out->success = 1;
        if (res) {
            out->inserted_count = res->result().inserted_count();
            out->inserted_id_json = dup_string(id_to_json(res->inserted_id()));
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
        auto parsed =
            nlohmann::json::parse(docs_json_array ? docs_json_array : "[]");
        std::vector<bsoncxx::document::value> docs;
        for (auto& d : parsed) {
            docs.push_back(bsoncxx::from_json(d.dump()));
        }
        auto res = coll.insert_many(docs);
        out->success = 1;
        if (res) {
            out->inserted_count = res->result().inserted_count();
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
                    out->upserted_id_json =
                        dup_string(id_to_json(id->get_value()));
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
        auto parsed =
            nlohmann::json::parse(pipeline_json ? pipeline_json : "[]");
        std::vector<bsoncxx::document::value> stages;
        for (auto& s : parsed) {
            stages.push_back(bsoncxx::from_json(s.dump()));
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
        out->inserted_id_json = dup_string(
            bsoncxx::to_json(res.view()));  // reply as "name created"
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
        auto coll =
            conn->client->database(conn->database_name).collection(collection);
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
}  // namespace

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
