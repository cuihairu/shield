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

#include "shield/plugin/abi.h"
#include "shield/plugin/leaderboard.h"

#include <sw/redis++/redis++.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

struct BoardConfig {
    std::vector<shield_leaderboard_field_def> field_defs;
    std::vector<int> field_bits;  // per-field bit widths
    bool primary_desc = true;     // direction of field 0 (drives top_n)
};

// shield_leaderboard_conn is opaque in leaderboard.h; concrete layout here.
struct shield_leaderboard_conn {
    std::shared_ptr<sw::redis::Redis> redis;
    std::unordered_map<std::string, BoardConfig> boards;
    std::mutex boards_mu;
};

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
                    std::free(r->entries[i].field_names);
                }
                if (r->entries[i].field_values)
                    std::free(r->entries[i].field_values);
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
                std::free(e->field_names);
            }
            if (e->field_values) std::free(e->field_values);
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
// v1 ABI entry
// ---------------------------------------------------------------------------
namespace {

struct lb_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int lb_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out,
              shield_error_v1* err) {
    (void)err;
    auto* inst = new lb_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_LEADERBOARD_INTERFACE) == 0)
            return &lb_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete reinterpret_cast<lb_instance*>(self);
    };
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
