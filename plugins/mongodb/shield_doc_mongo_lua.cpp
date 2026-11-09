// [SHIELD_PLUGIN] database.mongodb — Lua facade: error tables, the
// JSON<->Lua converters, the session/instance proxies and the
// shield.database.mongodb namespace installer.
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
// Error table construction (Lua side).
// ---------------------------------------------------------------------------
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// ---------------------------------------------------------------------------
// Lua bindings.
//
// The proxy returned by shield.database.mongodb(binding) exposes the
// full document surface. All methods return (ok, result|err) pairs to match
// the SQL plugin convention.
//
// `transaction(fn)` wraps mongocxx client_session.with_transaction so the
// caller doesn't have to deal with retry/abort logic. The Lua callback
// receives a per-transaction proxy whose methods automatically route
// through the session.
// ---------------------------------------------------------------------------
shd::table make_error_table(shd::state_view lua, const std::exception& e) {
    return make_error_table(lua, map_exception(e), e.what());
}

// Convert a nlohmann::json value into a Lua object (recursive). Mirrors the
// shape used by shield.lua's json_to_lua — duplicated here to keep the
// plugin self-contained (no host runtime dependency).
shd::object json_to_lua(shd::state_view lua, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return shd::nil;
        case nlohmann::json::value_t::boolean:
            return shd::make_object(lua, j.get<bool>());
        case nlohmann::json::value_t::number_integer:
            return shd::make_object(lua, j.get<int64_t>());
        case nlohmann::json::value_t::number_unsigned:
            return shd::make_object(lua, j.get<int64_t>());
        case nlohmann::json::value_t::number_float:
            return shd::make_object(lua, j.get<double>());
        case nlohmann::json::value_t::string:
            return shd::make_object(lua, j.get<std::string>());
        case nlohmann::json::value_t::array: {
            shd::table t = lua.create_table();
            int i = 1;
            for (auto& el : j) {
                t[i++] = json_to_lua(lua, el);
            }
            return t;
        }
        case nlohmann::json::value_t::object: {
            shd::table t = lua.create_table();
            for (auto& [k, v] : j.items()) {
                t[k] = json_to_lua(lua, v);
            }
            return t;
        }
        case nlohmann::json::value_t::binary:
            return shd::nil;
        case nlohmann::json::value_t::discarded:
            return shd::nil;
    }
    return shd::nil;
}

// Convert a Lua object into nlohmann::json (recursive). Tables with
// 1..N integer keys become arrays; otherwise objects. Nested tables work.
nlohmann::json lua_to_json(const shd::object& obj) {
    if (!obj.valid() || obj.get_type() == shd::type::nil) return nullptr;
    switch (obj.get_type()) {
        case shd::type::boolean:
            return obj.as<bool>();
        case shd::type::number:
            return obj.as<double>();
        case shd::type::string:
            return obj.as<std::string>();
        case shd::type::table: {
            shd::table t = obj.as<shd::table>();
            // Detect array vs object: integer keys 1..N only => array.
            bool is_array = true;
            int max_index = 0;
            int count = 0;
            for (const auto& kv : t) {
                ++count;
                if (kv.first.get_type() == shd::type::number) {
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
            for (const auto& kv : t) {
                std::string key;
                if (kv.first.get_type() == shd::type::number) {
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
std::string lua_to_json_string(const shd::object& obj) {
    return lua_to_json(obj).dump();
}

// Build the per-instance Lua proxy. Each method acquires a pool client,
// runs the operation, and returns the result.
shd::table make_instance_proxy(shd::state_view lua, mongo_instance* inst);

// Build a per-transaction proxy: all methods route through the given
// client_session. Bound to the pool_guard's lifetime.
shd::table make_session_proxy(shd::state_view lua, mongo_instance* inst,
                              mongocxx::client* client,
                              mongocxx::client_session* session) {
    auto proxy = lua.create_table();

    auto run = [&](const std::string& collection,
                   std::function<void(mongocxx::collection&)> fn)
        -> std::pair<bool, std::string> {
        try {
            auto coll =
                client->database(resolve_database(inst)).collection(collection);
            fn(coll);
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    };
    (void)run;  // used inside lambdas below

    proxy.set_function(
        "find",
        [inst, client, session](shd::this_state s, std::string collection,
                                shd::object filter_obj,
                                shd::object opts_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc =
                    bsoncxx::from_json(lua_to_json_string(filter_obj));
                auto cursor = session ? coll.find(*session, filter_doc.view())
                                      : coll.find(filter_doc.view());
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(
                        nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "find_one",
        [inst, client, session](
            shd::this_state s, std::string collection,
            shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc =
                    bsoncxx::from_json(lua_to_json_string(filter_obj));
                auto maybe = session
                                 ? coll.find_one(*session, filter_doc.view())
                                 : coll.find_one(filter_doc.view());
                results.push_back(shd::make_object(lua, true));
                if (maybe) {
                    results.push_back(json_to_lua(
                        lua, nlohmann::json::parse(bsoncxx::to_json(*maybe))));
                } else {
                    results.push_back(shd::nil);
                }
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "insert_one",
        [inst, client, session](shd::this_state s, std::string collection,
                                shd::object doc_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto doc = bsoncxx::from_json(lua_to_json_string(doc_obj));
                auto res = session ? coll.insert_one(*session, doc.view())
                                   : coll.insert_one(doc.view());
                auto t = lua.create_table();
                t["inserted_count"] =
                    res ? static_cast<int64_t>(res->result().inserted_count())
                        : 0;
                if (res) {
                    t["inserted_id"] = json_to_lua(
                        lua,
                        nlohmann::json::parse(id_to_json(res->inserted_id())));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "insert_many",
        [inst, client, session](shd::this_state s, std::string collection,
                                shd::object docs_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto arr = lua_to_json(docs_obj);
                std::vector<bsoncxx::document::value> docs;
                for (auto& d : arr) {
                    docs.push_back(bsoncxx::from_json(d.dump()));
                }
                auto res = session ? coll.insert_many(*session, docs)
                                   : coll.insert_many(docs);
                auto t = lua.create_table();
                t["inserted_count"] =
                    res ? static_cast<int64_t>(res->result().inserted_count())
                        : 0;
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "update_one",
        [inst, client, session](shd::this_state s, std::string collection,
                                shd::object filter_obj, shd::object update_obj,
                                shd::object opts_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::options::update opts;
                if (opts_obj.valid() &&
                    opts_obj.get_type() == shd::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("upsert") && o["upsert"].is_boolean())
                        opts.upsert(o["upsert"].get<bool>());
                }
                auto res =
                    session
                        ? coll.update_one(
                              *session,
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view(),
                              bsoncxx::from_json(lua_to_json_string(update_obj))
                                  .view(),
                              opts)
                        : coll.update_one(
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view(),
                              bsoncxx::from_json(lua_to_json_string(update_obj))
                                  .view(),
                              opts);
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] =
                        static_cast<int64_t>(res->matched_count());
                    t["modified_count"] =
                        static_cast<int64_t>(res->modified_count());
                    if (res->upserted_count() > 0 && res->upserted_id()) {
                        t["upserted_id"] = json_to_lua(
                            lua, nlohmann::json::parse(id_to_json(
                                     res->upserted_id()->get_value())));
                    }
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "update_many",
        [inst, client, session](
            shd::this_state s, std::string collection, shd::object filter_obj,
            shd::object update_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res =
                    session
                        ? coll.update_many(
                              *session,
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view(),
                              bsoncxx::from_json(lua_to_json_string(update_obj))
                                  .view())
                        : coll.update_many(
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view(),
                              bsoncxx::from_json(lua_to_json_string(update_obj))
                                  .view());
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] =
                        static_cast<int64_t>(res->matched_count());
                    t["modified_count"] =
                        static_cast<int64_t>(res->modified_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "delete_one",
        [inst, client, session](
            shd::this_state s, std::string collection,
            shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res =
                    session
                        ? coll.delete_one(
                              *session,
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view())
                        : coll.delete_one(
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] =
                        static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "delete_many",
        [inst, client, session](
            shd::this_state s, std::string collection,
            shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto res =
                    session
                        ? coll.delete_many(
                              *session,
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view())
                        : coll.delete_many(
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] =
                        static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "count",
        [inst, client, session](
            shd::this_state s, std::string collection,
            shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                auto count =
                    session
                        ? coll.count_documents(
                              *session,
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view())
                        : coll.count_documents(
                              bsoncxx::from_json(lua_to_json_string(filter_obj))
                                  .view());
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<int64_t>(count)));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "aggregate",
        [inst, client, session](
            shd::this_state s, std::string collection,
            shd::object pipeline_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            try {
                auto coll = client->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::pipeline p;
                auto arr = lua_to_json(pipeline_obj);
                for (auto& s_doc : arr) {
                    p.append_stage(bsoncxx::from_json(s_doc.dump()).view());
                }
                auto cursor =
                    session ? coll.aggregate(*session, p) : coll.aggregate(p);
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(
                        nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    return proxy;
}

shd::table make_instance_proxy(shd::state_view lua, mongo_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "find",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj,
               shd::object /*opts_obj*/) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::from_json(
                    filter_obj.valid() ? lua_to_json_string(filter_obj) : "{}");
                auto cursor = coll.find(filter_doc.view());
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(
                        nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "find_one",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto filter_doc = bsoncxx::from_json(
                    filter_obj.valid() ? lua_to_json_string(filter_obj) : "{}");
                auto maybe = coll.find_one(filter_doc.view());
                results.push_back(shd::make_object(lua, true));
                if (maybe) {
                    results.push_back(json_to_lua(
                        lua, nlohmann::json::parse(bsoncxx::to_json(*maybe))));
                } else {
                    results.push_back(shd::nil);
                }
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "insert_one",
        [inst](shd::this_state s, std::string collection,
               shd::object doc_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto doc = bsoncxx::from_json(lua_to_json_string(doc_obj));
                auto res = coll.insert_one(doc.view());
                auto t = lua.create_table();
                t["inserted_count"] =
                    res ? static_cast<int64_t>(res->result().inserted_count())
                        : 0;
                if (res) {
                    t["inserted_id"] = json_to_lua(
                        lua,
                        nlohmann::json::parse(id_to_json(res->inserted_id())));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "insert_many",
        [inst](shd::this_state s, std::string collection,
               shd::object docs_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto arr = lua_to_json(docs_obj);
                std::vector<bsoncxx::document::value> docs;
                for (auto& d : arr) {
                    docs.push_back(bsoncxx::from_json(d.dump()));
                }
                auto res = coll.insert_many(docs);
                auto t = lua.create_table();
                t["inserted_count"] =
                    res ? static_cast<int64_t>(res->result().inserted_count())
                        : 0;
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "update_one",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj, shd::object update_obj,
               shd::object opts_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::options::update opts;
                if (opts_obj.valid() &&
                    opts_obj.get_type() == shd::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("upsert") && o["upsert"].is_boolean())
                        opts.upsert(o["upsert"].get<bool>());
                }
                auto res = coll.update_one(
                    bsoncxx::from_json(lua_to_json_string(filter_obj)).view(),
                    bsoncxx::from_json(lua_to_json_string(update_obj)).view(),
                    opts);
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] =
                        static_cast<int64_t>(res->matched_count());
                    t["modified_count"] =
                        static_cast<int64_t>(res->modified_count());
                    if (res->upserted_count() > 0 && res->upserted_id()) {
                        t["upserted_id"] = json_to_lua(
                            lua, nlohmann::json::parse(id_to_json(
                                     res->upserted_id()->get_value())));
                    }
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "update_many",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj,
               shd::object update_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.update_many(
                    bsoncxx::from_json(lua_to_json_string(filter_obj)).view(),
                    bsoncxx::from_json(lua_to_json_string(update_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["matched_count"] =
                        static_cast<int64_t>(res->matched_count());
                    t["modified_count"] =
                        static_cast<int64_t>(res->modified_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "delete_one",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.delete_one(
                    bsoncxx::from_json(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] =
                        static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "delete_many",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto res = coll.delete_many(
                    bsoncxx::from_json(lua_to_json_string(filter_obj)).view());
                auto t = lua.create_table();
                if (res) {
                    t["deleted_count"] =
                        static_cast<int64_t>(res->deleted_count());
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(t);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "count",
        [inst](shd::this_state s, std::string collection,
               shd::object filter_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto count = coll.count_documents(
                    bsoncxx::from_json(filter_obj.valid()
                                           ? lua_to_json_string(filter_obj)
                                           : "{}")
                        .view());
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<int64_t>(count)));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "aggregate",
        [inst](shd::this_state s, std::string collection,
               shd::object pipeline_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                mongocxx::pipeline p;
                auto arr = lua_to_json(pipeline_obj);
                for (auto& s_doc : arr) {
                    p.append_stage(bsoncxx::from_json(s_doc.dump()).view());
                }
                auto cursor = coll.aggregate(p);
                nlohmann::json docs = nlohmann::json::array();
                for (auto&& doc : cursor) {
                    docs.push_back(
                        nlohmann::json::parse(bsoncxx::to_json(doc)));
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(json_to_lua(lua, docs));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function(
        "create_index",
        [inst](shd::this_state s, std::string collection, shd::object keys_obj,
               shd::object opts_obj) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto coll = g.get()
                                ->database(resolve_database(inst))
                                .collection(collection);
                auto keys = bsoncxx::from_json(lua_to_json_string(keys_obj));
                mongocxx::options::index opts;
                if (opts_obj.valid() &&
                    opts_obj.get_type() == shd::type::table) {
                    auto o = lua_to_json(opts_obj);
                    if (o.contains("name") && o["name"].is_string())
                        opts.name(o["name"].get<std::string>());
                    if (o.contains("unique") && o["unique"].is_boolean())
                        opts.unique(o["unique"].get<bool>());
                }
                auto name = coll.create_index(keys.view(), opts);
                results.push_back(shd::make_object(lua, true));
                // The created-index document (name/key/version) as a Lua
                // table — same BSON -> JSON -> Lua path insert_one's
                // inserted_id uses.
                results.push_back(json_to_lua(
                    lua, nlohmann::json::parse(doc_to_json(name.view()))));
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }
        });

    proxy.set_function("drop_index",
                       [inst](shd::this_state s, std::string collection,
                              std::string index_name) -> shd::variadic_results {
                           shd::state_view lua(s);
                           shd::variadic_results results;
                           std::string err;
                           pool_guard g = acquire_client(inst, &err);
                           if (!g) {
                               results.push_back(shd::make_object(lua, false));
                               results.push_back(make_error_table(
                                   lua, "connection_failed", err));
                               return results;
                           }
                           try {
                               auto coll =
                                   g.get()
                                       ->database(resolve_database(inst))
                                       .collection(collection);
                               coll.indexes().drop_one(index_name);
                               results.push_back(shd::make_object(lua, true));
                               return results;
                           } catch (const std::exception& e) {
                               results.push_back(shd::make_object(lua, false));
                               results.push_back(make_error_table(lua, e));
                               return results;
                           }
                       });

    proxy.set_function(
        "transaction",
        [inst](shd::this_state s,
               shd::protected_function callback) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_client(inst, &err);
            if (!g) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            mongocxx::client* client = g.get();
            std::unique_ptr<mongocxx::client_session> session;
            try {
                session = std::make_unique<mongocxx::client_session>(
                    client->start_session());
                session->start_transaction();
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua, e));
                return results;
            }

            shd::table tx_proxy =
                make_session_proxy(lua, inst, client, session.get());

            shd::protected_function_result cb_res = callback(tx_proxy);
            bool commit = cb_res.valid();
            bool user_abort = false;
            bool saw_error = false;

            if (cb_res.valid()) {
                shd::object first = cb_res.get(0);
                if (first.is<bool>() && !first.as<bool>()) {
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
                results.push_back(shd::make_object(lua, false));
                if (tx_err_msg.empty()) {
                    if (saw_error) {
                        results.push_back(
                            make_error_table(lua, "transaction_rolled_back",
                                             "callback raised an error"));
                    } else {
                        results.push_back(
                            make_error_table(lua, "transaction_rolled_back",
                                             "callback returned false"));
                    }
                } else {
                    results.push_back(
                        make_error_table(lua, tx_err_code, tx_err_msg));
                }
                return results;
            }

            results.push_back(shd::make_object(lua, true));
            int n_returns = cb_res.return_count();
            for (int i = 0; i < n_returns; ++i) {
                results.push_back(cb_res.get<shd::object>(i));
            }
            return results;
        });

    return proxy;
}
}  // namespace

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.mongodb: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<mongo_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.mongodb: host binding resolver is null";
        }
        return 1;
    }
    shd::state_view lua(L);

    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table database =
        shield::plugins::get_or_create_subtable(shield, "database");

    shd::object existing = database["mongodb"];
    if (!existing.is<shd::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function(
            "__call",
            [host_api, ctx](
                shd::this_state s, shd::table /*self*/,
                std::optional<std::string> binding) -> shd::variadic_results {
                shd::state_view lua(s);
                shd::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                results.push_back(
                    shd::make_object(lua, make_instance_proxy(lua, inst)));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        database["mongodb"] = ns;
    }

    return 0;
}
