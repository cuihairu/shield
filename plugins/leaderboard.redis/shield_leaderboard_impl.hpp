// [SHIELD_PLUGIN] leaderboard.redis — internal cross-TU declarations
// for the plugin split.
//
// Not a public contract: the exported ABI stays in
// shield/plugin/leaderboard.h. The board-config structs, the bit-width
// constants and the instance struct move here verbatim so the
// shield_leaderboard*.cpp translation units share one layout; the
// file-local helpers shared across the split are defined flat
// (external linkage) in their own TU, one declaration each.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/redis.h"

constexpr int kDefaultFieldBits = 16;
constexpr int kMaxFieldBits = 64;

// Field names are owned by the config (std::string), never a borrowed
// const char* from the caller's config table: the C vtable's create_board
// and the Lua path both hand in caller-owned pointers that may be released
// after the call returns, so a borrowed name would dangle on later use.
struct BoardField {
    std::string name;
    shield_sort_direction dir;
};

struct BoardConfig {
    std::vector<BoardField> field_defs;
    std::vector<int> field_bits;  // per-field bit widths
    bool primary_desc = true;     // direction of field 0 (drives top_n)
};

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
    const shield_redis_v2* redis_driver = nullptr;
    void* redis_handle = nullptr;

    // Per-instance BoardConfig cache. Survives across calls (unlike the
    // vtable's per-conn cache) because the Lua proxy opens a fresh conn per
    // call. Guarded by boards_mu.
    std::unordered_map<std::string, BoardConfig> boards;
    std::mutex boards_mu;
};

// Defined in shield_leaderboard_exec.cpp: the composite-score
// codec shared byte-identical between the C vtable and the Lua proxy.
double encode_composite(const BoardConfig& bc, const char* const* field_names,
                        const double* field_values, int field_count);
void decode_composite(const BoardConfig& bc, double composite,
                      std::vector<double>& out_fields);

// Defined in shield_leaderboard_exec.cpp: the legacy C-ABI surface,
// served through get_interface() in lb_create.
const shield_leaderboard_v1& lb_vtable();

// Defined in shield_leaderboard_redis.cpp: the process-wide registry.
void register_instance(leaderboard_instance* inst);
void unregister_instance(const std::string& id);
leaderboard_instance* find_instance(const std::string& id);

// Defined in shield_leaderboard_redis.cpp: config extraction used by
// lb_create.
void parse_instance_config(leaderboard_instance* inst, const char* config_json);

// Defined in shield_leaderboard_lua.cpp: releases the heap-allocated
// field names of a cached BoardConfig (instance cache overwrite,
// board delete, instance shutdown).
void free_board_config_names(BoardConfig& bc);

// Defined in shield_leaderboard_lua.cpp: register_lua implementation,
// installed by lb_create on the shell.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);

// Defined in shield_leaderboard_redis.cpp: the pool-stats surface,
// served through get_interface() in lb_create.
const shield_pool_stats_v1& lb_pool_stats_vtable();
