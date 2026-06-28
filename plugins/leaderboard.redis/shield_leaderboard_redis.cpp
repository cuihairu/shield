// [SHIELD_PLUGIN] leaderboard.redis — Redis ZSET provider for
// shield.leaderboard.v1.
//
// v1 ABI. The v1 interface is centered on (board_name, player_id, fields[]):
// each board is a Redis ZSET keyed by board_name, members are player_ids,
// scores are encoded from the per-board field definitions. set_entry encodes
// the field_values into a single double via encode_composite() and ZADDs it.
// get_entry decodes the score back. top_n uses ZREVRANGE/ZRANGE depending on
// the primary field's sort direction.
//
// Composite encoding (carried over from legacy shield_redis.cpp): each field
// is masked to N bits and the fields are concatenated MSB-first into a single
// uint64_t, which is then stored as the ZSET score. Field order matters:
// earlier fields dominate. Bit widths default to 16 if the board config asks
// for 0; total bits must be <= 64.
//
// Lua autonomy: register_lua() installs shield.leaderboard.redis as a callable
// Lua table. Calling it with the binding name returns a proxy whose methods
// (create_board, set_entry, top_n, ...) call Redis directly but reuse the
// same encode_composite() / decode_composite() helpers the C vtable uses,
// so the composite-score encoding stays byte-identical across the C and Lua
// paths and both operate on the same Redis ZSET data.

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/redis.h"

#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// shield_leaderboard_conn is opaque in leaderboard.h; concrete layout here.
// Defined at global scope so lambda-to-function-pointer conversion sees the
// same type as the vtable declared in leaderboard.h.
struct BoardConfig {
    std::vector<shield_leaderboard_field_def> field_defs;
    std::vector<int> field_bits;  // per-field bit widths
    bool primary_desc = true;     // direction of field 0 (drives top_n)
};

struct shield_leaderboard_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    std::unordered_map<std::string, BoardConfig> boards;
    std::mutex boards_mu;
};

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

constexpr int kDefaultFieldBits = 16;
constexpr int kMaxFieldBits     = 64;

// Encode the player's field values into a composite score. See file header.
double encode_composite(const BoardConfig& bc,
                        const char* const* field_names,
                        const double* field_values, int field_count) {
    // Build a lookup so the caller's field order need not match the board's.
    // Missing fields contribute 0.
    std::vector<double> ordered(bc.field_defs.size(), 0.0);
    for (int i = 0; i < field_count; ++i) {
        if (!field_names || !field_names[i]) continue;
        for (size_t j = 0; j < bc.field_defs.size(); ++j) {
            if (bc.field_defs[j].name &&
                std::strcmp(bc.field_defs[j].name, field_names[i]) == 0) {
                ordered[j] = field_values[i];
                break;
            }
        }
    }
    uint64_t composite = 0;
    int total_bits = 0;
    for (int b : bc.field_bits) total_bits += b;
    if (total_bits > kMaxFieldBits) total_bits = kMaxFieldBits;
    int shift = total_bits;
    for (size_t i = 0; i < bc.field_defs.size(); ++i) {
        shift -= bc.field_bits[i];
        if (shift < 0) break;
        uint64_t mask = (1ULL << bc.field_bits[i]) - 1;
        uint64_t v = static_cast<uint64_t>(ordered[i]) & mask;
        composite |= (v << shift);
    }
    return static_cast<double>(composite);
}

void decode_composite(const BoardConfig& bc, double composite,
                      std::vector<double>& out_fields) {
    uint64_t val = static_cast<uint64_t>(composite);
    size_t n = bc.field_defs.size();
    out_fields.assign(n, 0.0);
    int total_bits = 0;
    for (int b : bc.field_bits) total_bits += b;
    int shift = total_bits;
    for (size_t i = 0; i < n; ++i) {
        shift -= bc.field_bits[i];
        if (shift < 0) break;
        uint64_t mask = (1ULL << bc.field_bits[i]) - 1;
        out_fields[i] = static_cast<double>((val >> shift) & mask);
    }
}

BoardConfig materialize_board(const shield_leaderboard_config* config) {
    BoardConfig bc;
    bc.field_defs.reserve(config->field_count);
    bc.field_bits.reserve(config->field_count);
    for (int i = 0; i < config->field_count; ++i) {
        const auto& src = config->field_defs[i];
        bc.field_defs.push_back({src.name, src.dir});
        // Bit width: derive from field name suffix "name:bits" if present,
        // otherwise default. Kept simple — production boards configure via
        // extra_json if they need per-field widths.
        bc.field_bits.push_back(kDefaultFieldBits);
        if (i == 0) bc.primary_desc = (src.dir == SHIELD_SORT_DESC);
    }
    if (bc.field_defs.empty()) {
        bc.field_defs.push_back({"score", SHIELD_SORT_DESC});
        bc.field_bits.push_back(kDefaultFieldBits);
        bc.primary_desc = true;
    }
    return bc;
}

// ---------------------------------------------------------------------------
// v1 leaderboard vtable
// ---------------------------------------------------------------------------
const shield_leaderboard_v1& lb_vtable() {
    static const shield_leaderboard_v1 v = {
        sizeof(shield_leaderboard_v1),
        SHIELD_LEADERBOARD_INTERFACE,
        "redis",
        "1.0.0",
        // connect
        [](const shield_leaderboard_connect_args* args,
           char* err_buf, int err_buf_size) -> shield_leaderboard_conn* {
            if (!args) return nullptr;
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = args->host && args->host[0] ? args->host : "localhost";
                opts.port = args->port > 0 ? args->port : 6379;
                if (args->password && args->password[0])
                    opts.password = args->password;
                opts.db = args->db > 0 ? args->db : 0;
                opts.connect_timeout = std::chrono::milliseconds(
                    args->connect_timeout_ms > 0 ? args->connect_timeout_ms : 5000);
                opts.socket_timeout = std::chrono::milliseconds(
                    args->command_timeout_ms > 0 ? args->command_timeout_ms : 5000);
                auto redis = std::make_shared<sw::redis::Redis>(opts);
                redis->ping();
                return new shield_leaderboard_conn{redis};
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            }
        },
        // disconnect
        [](shield_leaderboard_conn* c) { delete c; },
        // create_board
        [](shield_leaderboard_conn* c,
           const shield_leaderboard_config* config,
           char* err_buf, int err_buf_size) -> int {
            if (!c || !config || !config->name) return -1;
            try {
                std::lock_guard<std::mutex> lock(c->boards_mu);
                c->boards[std::string(config->name)] = materialize_board(config);
                return 0;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                return -1;
            }
        },
        // delete_board
        [](shield_leaderboard_conn* c, const char* board_name) -> int {
            if (!c || !c->redis || !board_name) return -1;
            try {
                c->redis->del(std::string(board_name));
                std::lock_guard<std::mutex> lock(c->boards_mu);
                c->boards.erase(std::string(board_name));
                return 0;
            } catch (...) { return -1; }
        },
        // set_entry
        [](shield_leaderboard_conn* c, const char* board_name,
           const char* player_id,
           const char* const* field_names, const double* field_values,
           int field_count) -> int {
            if (!c || !c->redis || !board_name || !player_id) return -1;
            try {
                BoardConfig bc;
                {
                    std::lock_guard<std::mutex> lock(c->boards_mu);
                    auto it = c->boards.find(std::string(board_name));
                    if (it == c->boards.end()) {
                        bc.field_defs.push_back({"score", SHIELD_SORT_DESC});
                        bc.field_bits.push_back(kDefaultFieldBits);
                        bc.primary_desc = true;
                    } else {
                        bc = it->second;
                    }
                }
                double score = encode_composite(bc, field_names,
                                                field_values, field_count);
                c->redis->zadd(std::string(board_name),
                               std::string(player_id), score);
                return 0;
            } catch (...) { return -1; }
        },
        // remove_entry
        [](shield_leaderboard_conn* c, const char* board_name,
           const char* player_id) -> int {
            if (!c || !c->redis || !board_name || !player_id) return -1;
            try {
                c->redis->zrem(std::string(board_name),
                               std::string(player_id));
                return 0;
            } catch (...) { return -1; }
        },
        // get_entry
        [](shield_leaderboard_conn* c, const char* board_name,
           const char* player_id,
           shield_leaderboard_entry* out) -> int {
            if (!c || !c->redis || !board_name || !player_id || !out)
                return -1;
            try {
                auto score = c->redis->zscore(std::string(board_name),
                                              std::string(player_id));
                if (!score) {
                    out->player_id = nullptr;
                    out->field_names = nullptr;
                    out->field_values = nullptr;
                    out->field_count = 0;
                    out->rank = 0;
                    return -1;
                }
                BoardConfig bc;
                {
                    std::lock_guard<std::mutex> lock(c->boards_mu);
                    auto it = c->boards.find(std::string(board_name));
                    if (it != c->boards.end()) bc = it->second;
                }
                if (bc.field_defs.empty()) {
                    bc.field_defs.push_back({"score", SHIELD_SORT_DESC});
                    bc.field_bits.push_back(kDefaultFieldBits);
                    bc.primary_desc = true;
                }
                std::vector<double> ordered;
                decode_composite(bc, *score, ordered);

                int n = static_cast<int>(bc.field_defs.size());
                out->field_count = n;
                out->player_id = dup_string(player_id);
                char** names = static_cast<char**>(
                    std::calloc(n, sizeof(char*)));
                double* vals = static_cast<double*>(
                    std::calloc(n, sizeof(double)));
                for (int i = 0; i < n; ++i) {
                    names[i] = dup_string(bc.field_defs[i].name);
                    vals[i] = ordered[i];
                }
                out->field_names = names;
                out->field_values = vals;
                out->rank = 0;
                return 0;
            } catch (...) { return -1; }
        },
        // get_rank
        [](shield_leaderboard_conn* c, const char* board_name,
           const char* player_id, int64_t* out_rank) -> int {
            if (!c || !c->redis || !board_name || !player_id || !out_rank)
                return -1;
            try {
                auto r = c->redis->zrank(std::string(board_name),
                                         std::string(player_id));
                if (!r) { *out_rank = 0; return -1; }
                *out_rank = static_cast<int64_t>(*r) + 1;
                return 0;
            } catch (...) { *out_rank = 0; return -1; }
        },
        // top_n
        [](shield_leaderboard_conn* c, const char* board_name, int n,
           shield_leaderboard_result* out) -> int {
            if (!c || !c->redis || !board_name || n <= 0) {
                out->success = 0;
                out->error_code = "invalid_args";
                out->error_msg = "";
                out->entries = nullptr;
                out->entry_count = 0;
                return -1;
            }
            try {
                std::vector<std::pair<std::string, double>> rows;
                if (c->boards.count(std::string(board_name))) {
                    bool desc;
                    {
                        std::lock_guard<std::mutex> lock(c->boards_mu);
                        desc = c->boards[std::string(board_name)].primary_desc;
                    }
                    if (desc) {
                        c->redis->zrevrange(std::string(board_name), 0,
                                            n - 1, std::back_inserter(rows));
                    } else {
                        c->redis->zrange(std::string(board_name), 0,
                                         n - 1, std::back_inserter(rows));
                    }
                } else {
                    c->redis->zrevrange(std::string(board_name), 0,
                                        n - 1, std::back_inserter(rows));
                }
                out->success = 1;
                out->error_code = "";
                out->error_msg = "";
                out->entry_count = static_cast<int>(rows.size());
                if (rows.empty()) { out->entries = nullptr; return 0; }
                out->entries = static_cast<shield_leaderboard_entry*>(
                    std::calloc(rows.size(), sizeof(shield_leaderboard_entry)));
                int idx = 0;
                for (const auto& [member, score] : rows) {
                    out->entries[idx].player_id = dup_string(member.c_str());
                    out->entries[idx].field_names = nullptr;
                    out->entries[idx].field_values = nullptr;
                    out->entries[idx].field_count = 0;
                    out->entries[idx].rank = idx + 1;
                    (void)score;
                    ++idx;
                }
                return 0;
            } catch (const std::exception& e) {
                out->success = 0;
                out->error_code = "lb_query_failed";
                out->error_msg = dup_string(e.what());
                out->entries = nullptr;
                out->entry_count = 0;
                return -1;
            }
        },
        // free_result
        [](shield_leaderboard_result* r) {
            if (!r || !r->entries) return;
            for (int i = 0; i < r->entry_count; ++i) {
                if (r->entries[i].player_id)
                    std::free(const_cast<char*>(r->entries[i].player_id));
                if (r->entries[i].field_names) {
                    for (int j = 0; j < r->entries[i].field_count; ++j) {
                        if (r->entries[i].field_names[j])
                            std::free(const_cast<char*>(
                                r->entries[i].field_names[j]));
                    }
                    std::free(const_cast<char**>(r->entries[i].field_names));
                }
                if (r->entries[i].field_values)
                    std::free(const_cast<double*>(r->entries[i].field_values));
            }
            std::free(r->entries);
            r->entries = nullptr;
            r->entry_count = 0;
        },
        // free_entry
        [](shield_leaderboard_entry* e) {
            if (!e) return;
            if (e->player_id) std::free(const_cast<char*>(e->player_id));
            if (e->field_names) {
                for (int i = 0; i < e->field_count; ++i)
                    if (e->field_names[i])
                        std::free(const_cast<char*>(e->field_names[i]));
                std::free(const_cast<char**>(e->field_names));
            }
            if (e->field_values) std::free(const_cast<double*>(e->field_values));
            e->player_id = nullptr;
            e->field_names = nullptr;
            e->field_values = nullptr;
            e->field_count = 0;
        },
    };
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and registers itself in a process-wide map so the Lua
// callable namespace `shield.leaderboard.redis(binding)` can resolve it.
// The C++ vtable is still served through get_interface() for host code that
// goes through LeaderboardPool / get_by_binding<shield_leaderboard_v1>.
//
// The Lua proxy goes straight to Redis via open_redis() (one fresh connection
// per call — redis++ pools internally) and uses the same encode_composite() /
// decode_composite() helpers the C vtable uses, so the composite-score
// encoding stays byte-identical across the C and Lua paths.
// ---------------------------------------------------------------------------
namespace {

struct leaderboard_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
    std::string password;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 5000;
    // Optional redis.driver dependency.
    const shield_redis_v1* redis_driver = nullptr;
    void* redis_handle = nullptr;

    // Per-instance BoardConfig cache. Survives across calls (unlike the
    // vtable's per-conn cache) because the Lua proxy opens a fresh conn per
    // call. Guarded by boards_mu.
    std::unordered_map<std::string, BoardConfig> boards;
    std::mutex boards_mu;
};

// Process-wide registry: instance_id -> leaderboard_instance*. The callable
// Lua table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, leaderboard_instance*>& instances_map() {
    static std::map<std::string, leaderboard_instance*> m;
    return m;
}

void register_instance(leaderboard_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
leaderboard_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the known keys and fall
// back to defaults for anything missing.
void parse_instance_config(leaderboard_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string()) {
            inst->host = j["host"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            inst->port = j["port"].get<int>();
        }
        if (j.contains("db") && j["db"].is_number_integer()) {
            inst->db = j["db"].get<int>();
        }
        if (j.contains("password") && j["password"].is_string()) {
            inst->password = j["password"].get<std::string>();
        }
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer()) {
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

// ---------------------------------------------------------------------------
// Lua helpers.
//
// Redis connections are pooled internally by redis++; open_redis() borrows a
// fresh connection from the pool each call. Returns nullptr on failure (and
// fills an error message).
// ---------------------------------------------------------------------------

std::shared_ptr<sw::redis::Redis> open_redis(const leaderboard_instance* inst,
                                             std::string* err_msg) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = inst->host.empty() ? "localhost" : inst->host;
        opts.port = inst->port > 0 ? inst->port : 6379;
        if (!inst->password.empty()) opts.password = inst->password;
        opts.db = inst->db > 0 ? inst->db : 0;
        opts.connect_timeout = std::chrono::milliseconds(
            inst->connect_timeout_ms > 0 ? inst->connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            inst->command_timeout_ms > 0 ? inst->command_timeout_ms : 5000);
        auto redis = std::make_shared<sw::redis::Redis>(opts);
        redis->ping();
        return redis;
    } catch (const std::exception& e) {
        if (err_msg) *err_msg = e.what();
        return nullptr;
    }
}

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.leaderboard.* facade.
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Free heap-allocated names owned by a cached BoardConfig. The instance cache
// deep-copies field names (so they outlive the Lua config table) — we must
// release them on overwrite / board delete / instance shutdown.
void free_board_config_names(BoardConfig& bc) {
    for (auto& fd : bc.field_defs) {
        if (fd.name) {
            std::free(const_cast<char*>(fd.name));
            fd.name = nullptr;
        }
    }
}

// Resolve a board's BoardConfig snapshot from the instance-level cache. If
// the board was never registered via create_board, returns a synthetic
// default (single "score" field, descending) — mirrors the C vtable's
// fallback behavior so the Lua and C paths agree on encoding. The returned
// BoardConfig's field_names point at string literals ("score"), not heap
// memory, so callers must NOT free them.
BoardConfig resolve_board_config(leaderboard_instance* inst,
                                 const std::string& board_name) {
    std::lock_guard<std::mutex> lock(inst->boards_mu);
    auto it = inst->boards.find(board_name);
    if (it != inst->boards.end()) return it->second;
    BoardConfig bc;
    bc.field_defs.push_back({"score", SHIELD_SORT_DESC});
    bc.field_bits.push_back(kDefaultFieldBits);
    bc.primary_desc = true;
    return bc;
}

// Build a per-instance proxy table that opens a fresh connection per call.
sol::table make_instance_proxy(sol::state_view lua,
                               leaderboard_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("create_board",
        [inst](sol::this_state s, std::string board_name,
               sol::table def) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            // No Redis I/O needed: create_board only populates the per-instance
            // BoardConfig cache used for composite-score encoding/decoding.
            // (The C vtable's create_board is likewise a pure cache populate.)

            // Translate def = {fields={{name=,order=}, ...}, sort="asc"|"desc"}
            // into our internal BoardConfig (with heap-owned field names).
            std::vector<std::pair<std::string, shield_sort_direction>> parsed;
            sol::optional<sol::table> fields = def["fields"];
            if (fields) {
                for (auto& kv : *fields) {
                    sol::object v = kv.second;
                    if (!v.is<sol::table>()) continue;
                    sol::table fd = v.as<sol::table>();
                    std::string name = fd.get_or("name",
                                                  std::string(""));
                    std::string order = fd.get_or("order",
                                                  std::string("desc"));
                    if (name.empty()) continue;
                    shield_sort_direction dir =
                        (order == "asc") ? SHIELD_SORT_ASC : SHIELD_SORT_DESC;
                    parsed.emplace_back(std::move(name), dir);
                }
            }
            if (parsed.empty()) {
                // Fallback single-field board — matches vtable default.
                parsed.emplace_back("score", SHIELD_SORT_DESC);
            }
            // Top-level sort hint (asc/desc) overrides field 0's direction so
            // callers can say `sort = "asc"` at the board level for intuitive
            // top_n ordering.
            sol::optional<std::string> top_sort = def["sort"];
            if (top_sort && !top_sort->empty()) {
                parsed[0].second =
                    (*top_sort == "asc") ? SHIELD_SORT_ASC : SHIELD_SORT_DESC;
            }

            // Persist into the instance-level cache. Heap-duplicate names so
            // they survive past this lambda; the instance cache frees them on
            // overwrite / board delete / shutdown.
            try {
                std::lock_guard<std::mutex> lock(inst->boards_mu);
                auto it = inst->boards.find(board_name);
                if (it != inst->boards.end()) {
                    free_board_config_names(it->second);
                }
                BoardConfig& dst = inst->boards[board_name];
                dst.field_defs.clear();
                dst.field_bits.clear();
                dst.primary_desc = (parsed[0].second == SHIELD_SORT_DESC);
                for (const auto& [name, dir] : parsed) {
                    dst.field_defs.push_back({dup_string(name.c_str()), dir});
                    dst.field_bits.push_back(kDefaultFieldBits);
                }
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_create_failed", e.what()));
                return results;
            }
            results.push_back(sol::make_object(lua, true));
            return results;
        });

    proxy.set_function("delete_board",
        [inst](sol::this_state s,
               std::string board_name) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            try {
                redis->del(board_name);
                // Drop the instance-level cache entry so future per-call
                // queries stop using the (now-deleted) board's encoding.
                {
                    std::lock_guard<std::mutex> lock(inst->boards_mu);
                    auto it = inst->boards.find(board_name);
                    if (it != inst->boards.end()) {
                        free_board_config_names(it->second);
                        inst->boards.erase(it);
                    }
                }
                results.push_back(sol::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("set_entry",
        [inst](sol::this_state s, std::string board_name,
               std::string player_id,
               sol::table fields_table) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            // Translate the Lua fields table {field_name = number, ...} into
            // parallel C arrays. encode_composite looks up each field by name,
            // so caller-supplied order does not matter.
            std::vector<std::string> names_storage;
            std::vector<double> values_storage;
            for (auto& kv : fields_table) {
                sol::object key = kv.first;
                sol::object val = kv.second;
                if (key.get_type() != sol::type::string) continue;
                if (!val.is<double>() && !val.is<lua_Integer>() &&
                    !val.is<bool>()) continue;
                names_storage.push_back(key.as<std::string>());
                if (val.is<bool>()) {
                    values_storage.push_back(val.as<bool>() ? 1.0 : 0.0);
                } else if (val.is<lua_Integer>()) {
                    values_storage.push_back(
                        static_cast<double>(val.as<lua_Integer>()));
                } else {
                    values_storage.push_back(val.as<double>());
                }
            }
            std::vector<const char*> names_ptr(names_storage.size(), nullptr);
            for (size_t i = 0; i < names_storage.size(); ++i) {
                names_ptr[i] = names_storage[i].c_str();
            }
            // Resolve the board's BoardConfig from the per-instance cache and
            // encode via the SAME encode_composite() the C vtable uses.
            BoardConfig bc = resolve_board_config(inst, board_name);
            double score = encode_composite(bc, names_ptr.data(),
                                            values_storage.data(),
                                            static_cast<int>(names_storage.size()));
            try {
                redis->zadd(board_name, player_id, score);
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
                return results;
            }
            results.push_back(sol::make_object(lua, true));
            return results;
        });

    proxy.set_function("remove_entry",
        [inst](sol::this_state s, std::string board_name,
               std::string player_id) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            try {
                redis->zrem(board_name, player_id);
                results.push_back(sol::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("get_entry",
        [inst](sol::this_state s, std::string board_name,
               std::string player_id) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            try {
                auto score_opt = redis->zscore(board_name, player_id);
                if (!score_opt) {
                    // Player not present. (true, nil) so callers can
                    // distinguish "board ok, no entry" from a hard failure.
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua, sol::nil));
                    return results;
                }
                BoardConfig bc = resolve_board_config(inst, board_name);
                std::vector<double> decoded;
                decode_composite(bc, *score_opt, decoded);
                auto entry_table = lua.create_table();
                entry_table["player_id"] = player_id;
                auto fields = lua.create_table();
                for (size_t i = 0; i < bc.field_defs.size() &&
                                   i < decoded.size(); ++i) {
                    if (bc.field_defs[i].name) {
                        fields[bc.field_defs[i].name] = decoded[i];
                    }
                }
                entry_table["fields"] = fields;
                results.push_back(sol::make_object(lua, true));
                results.push_back(entry_table);
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("get_rank",
        [inst](sol::this_state s, std::string board_name,
               std::string player_id) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            try {
                auto r = redis->zrank(board_name, player_id);
                if (!r) {
                    // Not ranked — ok=true with rank=0 (matches vtable).
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua,
                        static_cast<lua_Integer>(0)));
                } else {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua,
                        static_cast<lua_Integer>(*r + 1)));
                }
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("top_n",
        [inst](sol::this_state s, std::string board_name,
               int n) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            // The vtable's top_n returns only player_id + rank (it drops the
            // ZSET score), which would force Lua callers to re-query per
            // player to recover field values. For the Lua path we go directly
            // to Redis (ZRANGE WITHSCORES) and decode the composite score here
            // using the same decode_composite() the vtable uses, so callers
            // get the full per-field snapshot in one call.
            try {
                if (n <= 0) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, "invalid_args", "n must be positive"));
                    return results;
                }
                BoardConfig bc = resolve_board_config(inst, board_name);
                std::vector<std::pair<std::string, double>> rows;
                if (bc.primary_desc) {
                    redis->zrevrange(board_name, 0, n - 1,
                                     std::back_inserter(rows));
                } else {
                    redis->zrange(board_name, 0, n - 1,
                                  std::back_inserter(rows));
                }
                auto entries = lua.create_table();
                int idx = 1;
                for (const auto& [member, score] : rows) {
                    auto row = lua.create_table();
                    row["player_id"] = member;
                    row["rank"] = static_cast<lua_Integer>(idx);
                    auto fields = lua.create_table();
                    std::vector<double> decoded;
                    decode_composite(bc, score, decoded);
                    for (size_t i = 0; i < bc.field_defs.size() &&
                                       i < decoded.size(); ++i) {
                        if (bc.field_defs[i].name) {
                            fields[bc.field_defs[i].name] = decoded[i];
                        }
                    }
                    row["fields"] = fields;
                    entries[idx++] = row;
                }
                results.push_back(sol::make_object(lua, true));
                results.push_back(entries);
                return results;
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "lb_query_failed", e.what()));
                return results;
            }
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.leaderboard.redis. Lua passes a binding logical name; host config
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "leaderboard.redis: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<leaderboard_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message =
                "leaderboard.redis: host binding resolver is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.leaderboard.redis.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto leaderboard = shield["leaderboard"].get_or_create<sol::table>();

    sol::object existing = leaderboard["redis"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function("__call",
            [host_api, ctx](sol::this_state s, sol::table /*self*/,
               std::string binding) -> sol::object {
                sol::state_view lua(s);
                const char* instance_id =
                    host_api->binding_instance_id(ctx, binding.c_str());
                if (!instance_id) return sol::nil;
                auto* inst = find_instance(instance_id);
                if (!inst) return sol::nil;
                return sol::make_object(lua, make_instance_proxy(lua, inst));
            });
        ns[sol::metatable_key] = mt;
        leaderboard["redis"] = ns;
    }

    return 0;
}

int lb_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out,
              shield_error_v1* err) {
    (void)err;
    auto* inst = new leaderboard_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_LEADERBOARD_INTERFACE) == 0)
            return &lb_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1*) -> int {
        auto* li = reinterpret_cast<leaderboard_instance*>(self);
        // Try to get redis.driver dependency (optional).
        if (li->host_api && li->host_api->dependency) {
            auto* drv = static_cast<const shield_redis_v1*>(
                li->host_api->dependency(li->ctx, "redis", SHIELD_REDIS_V1));
            if (drv && drv->connect) {
                char err_buf[256] = {};
                void* handle = drv->connect(nullptr, err_buf, sizeof(err_buf));
                if (handle) {
                    li->redis_driver = drv;
                    li->redis_handle = handle;
                }
            }
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* li = reinterpret_cast<leaderboard_instance*>(self);
        if (li->redis_driver && li->redis_handle) {
            li->redis_driver->disconnect(li->redis_handle);
            li->redis_handle = nullptr;
            li->redis_driver = nullptr;
        }
        unregister_instance(li->instance_id);
        {
            std::lock_guard<std::mutex> lock(li->boards_mu);
            for (auto& [name, bc] : li->boards) {
                free_board_config_names(bc);
            }
            li->boards.clear();
        }
        delete li;
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
        "leaderboard.redis",
        "1.0.0",
        lb_create,
    };
    return &abi;
}
