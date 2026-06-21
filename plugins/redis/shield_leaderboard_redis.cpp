// [SHIELD_PLUGIN] Leaderboard plugin using Redis ZSET
//
// Uses shield_redis_plugin for sorted set operations.
// Supports multi-field sorting via composite score encoding.

#include "shield/plugin/plugin.h"
#include "shield/plugin/leaderboard_plugin.h"
#include "shield/plugin/redis_plugin.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace {

const shield_redis_plugin* g_redis = nullptr;
shield_redis_conn* g_conn = nullptr;

// Board configs stored in memory.
struct BoardConfig {
    std::string name;
    std::vector<shield_leaderboard_field_def> field_defs;
};
std::unordered_map<std::string, BoardConfig> g_boards;

int lb_init(const void* redis_plugin, char* err_buf, int err_buf_size) {
    if (!redis_plugin) {
        if (err_buf && err_buf_size > 0)
            std::strncpy(err_buf, "redis plugin not provided", err_buf_size - 1);
        return -1;
    }
    g_redis = static_cast<const shield_redis_plugin*>(redis_plugin);
    return 0;
}

void lb_shutdown() {
    g_redis = nullptr;
    g_conn = nullptr;
    g_boards.clear();
}

int lb_add_score(const char* board_name, const char* player_id, double score) {
    if (!g_redis || !g_conn) return -1;
    // Use the board name as the Redis key.
    return g_redis->zadd(g_conn, board_name, score, player_id);
}

int lb_remove(const char* board_name, const char* player_id) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->zrem(g_conn, board_name, player_id);
}

int lb_get_score(const char* board_name, const char* player_id, double* out) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->zscore(g_conn, board_name, player_id, out);
}

int lb_get_rank(const char* board_name, const char* player_id, int64_t* out) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->zrank(g_conn, board_name, player_id, out);
}

int lb_get_top(const char* board_name, int limit,
               shield_leaderboard_result* out) {
    if (!g_redis || !g_conn) {
        out->success = 0;
        out->error_code = "not_initialized";
        return -1;
    }

    shield_redis_zentry* entries = nullptr;
    int count = 0;
    int r = g_redis->zrevrange(g_conn, board_name, 0, limit - 1, &entries, &count);

    out->success = (r == 0) ? 1 : 0;
    out->error_code = "";
    out->error_msg = "";
    out->entry_count = count;

    if (r == 0 && count > 0) {
        out->entries = static_cast<shield_leaderboard_entry*>(
            std::calloc(count, sizeof(shield_leaderboard_entry)));
        for (int i = 0; i < count; ++i) {
            out->entries[i].player_id = entries[i].member;
            out->entries[i].score = entries[i].score;
            out->entries[i].rank = i + 1;
            // Don't free member strings - they're now owned by the entry.
        }
        // Free the zentry array but not the member strings.
        std::free(entries);
    } else {
        out->entries = nullptr;
    }
    return r;
}

int lb_get_around(const char* board_name, const char* player_id,
                  int range, shield_leaderboard_result* out) {
    if (!g_redis || !g_conn) {
        out->success = 0;
        return -1;
    }

    // Get player's rank first.
    int64_t rank = 0;
    int r = g_redis->zrank(g_conn, board_name, player_id, &rank);
    if (r != 0 || rank == 0) {
        out->success = 0;
        out->error_code = "player_not_found";
        out->entry_count = 0;
        return -1;
    }

    int64_t start = std::max<int64_t>(0, rank - 1 - range);
    int64_t stop = rank - 1 + range;

    shield_redis_zentry* entries = nullptr;
    int count = 0;
    r = g_redis->zrange(g_conn, board_name, start, stop, &entries, &count);

    out->success = (r == 0) ? 1 : 0;
    out->entry_count = count;
    if (r == 0 && count > 0) {
        out->entries = static_cast<shield_leaderboard_entry*>(
            std::calloc(count, sizeof(shield_leaderboard_entry)));
        for (int i = 0; i < count; ++i) {
            out->entries[i].player_id = entries[i].member;
            out->entries[i].score = entries[i].score;
            out->entries[i].rank = start + i + 1;
        }
        std::free(entries);
    } else {
        out->entries = nullptr;
    }
    return r;
}

int lb_get_range(const char* board_name, int64_t start, int64_t end,
                 shield_leaderboard_result* out) {
    if (!g_redis || !g_conn) { out->success = 0; return -1; }

    shield_redis_zentry* entries = nullptr;
    int count = 0;
    int r = g_redis->zrange(g_conn, board_name, start - 1, end - 1, &entries, &count);

    out->success = (r == 0) ? 1 : 0;
    out->entry_count = count;
    if (r == 0 && count > 0) {
        out->entries = static_cast<shield_leaderboard_entry*>(
            std::calloc(count, sizeof(shield_leaderboard_entry)));
        for (int i = 0; i < count; ++i) {
            out->entries[i].player_id = entries[i].member;
            out->entries[i].score = entries[i].score;
            out->entries[i].rank = start + i;
        }
        std::free(entries);
    } else {
        out->entries = nullptr;
    }
    return r;
}

int lb_delete_board(const char* board_name) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->del(g_conn, board_name);
}

int lb_count(const char* board_name, int64_t* out) {
    if (!g_redis || !g_conn) { *out = 0; return -1; }
    return g_redis->zcount(g_conn, board_name, out);
}

int lb_save(const char*) { return 0; }  // Redis is already persistent
int lb_load(const char*) { return 0; }  // Redis data is already loaded

void lb_free_result(shield_leaderboard_result* result) {
    if (!result) return;
    if (result->entries) {
        for (int i = 0; i < result->entry_count; ++i) {
            if (result->entries[i].player_id)
                std::free(const_cast<char*>(result->entries[i].player_id));
        }
        std::free(result->entries);
        result->entries = nullptr;
    }
    result->entry_count = 0;
}

const shield_leaderboard_plugin g_lb_plugin = {
    SHIELD_LEADERBOARD_ABI_VERSION,
    "redis_leaderboard",
    "1.0.0",

    nullptr,  // init handled by plugin init
    nullptr,  // shutdown handled by plugin shutdown

    lb_add_score,
    lb_remove,
    lb_get_score,
    lb_get_rank,
    lb_get_top,
    lb_get_around,
    lb_get_range,
    lb_delete_board,
    lb_count,

    lb_save,
    lb_load,

    lb_free_result,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_LEADERBOARD,
    "shield_leaderboard",
    "1.0.0",
    "Leaderboard plugin using Redis ZSET",
    "Shield",

    [](const shield_host_t host, const shield_host_api* api,
       const shield_plugin_config*, char* err, int err_len) -> int {
        const shield_plugin* redis_p = api->find_plugin(host, "shield_redis");
        if (!redis_p) {
            if (err && err_len > 0) std::strncpy(err, "shield_redis not loaded", err_len - 1);
            return -1;
        }
        return lb_init(redis_p->vtable, err, err_len);
    },

    []() { lb_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"leaderboard", "1.0.0", "Redis ZSET leaderboard"};
        return &cap;
    },

    &g_lb_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
