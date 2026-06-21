// [SHIELD_PLUGIN] ELO-based matchmaking plugin
//
// Implements shield_matchmaking_plugin for skill-based matchmaking
// using ELO rating system.

#include "shield/plugin/plugin.h"
#include "shield/plugin/matchmaking_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct PlayerEntry {
    std::string player_id;
    double rating;
    std::string party_id;
    std::string region;
};

struct MatchConfig {
    int max_players_per_match = 2;
    int match_timeout_seconds = 30;
    double rating_tolerance = 200.0;  // max rating difference for a match
};

MatchConfig g_config;
std::unordered_map<std::string, PlayerEntry> g_queue;  // player_id -> entry
std::mutex g_mutex;
uint64_t g_next_match_id = 1;

int mm_init(const char* config_json, char* err_buf, int err_buf_size) {
    return 0;
}

void mm_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
}

int mm_enqueue(shield_matchmaking_conn*, const shield_matchmaking_player* player) {
    if (!player || !player->player_id) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    PlayerEntry entry;
    entry.player_id = player->player_id;
    entry.rating = player->rating;
    entry.party_id = player->party_id ? player->party_id : "";
    entry.region = player->region ? player->region : "";
    g_queue[player->player_id] = std::move(entry);
    return 0;
}

int mm_dequeue(shield_matchmaking_conn*, const char* player_id) {
    if (!player_id) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.erase(player_id);
    return 0;
}

int mm_is_queued(shield_matchmaking_conn*, const char* player_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_queue.count(player_id) > 0 ? 1 : 0;
}

int mm_try_match(shield_matchmaking_conn*, shield_matchmaking_result* out) {
    std::lock_guard<std::mutex> lock(g_mutex);

    // Simple greedy matching: sort by rating, pair adjacent players
    // within tolerance.
    std::vector<PlayerEntry*> sorted;
    for (auto& [id, p] : g_queue) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(),
              [](const PlayerEntry* a, const PlayerEntry* b) {
                  return a->rating < b->rating;
              });

    std::vector<std::vector<PlayerEntry*>> matches;
    size_t i = 0;
    while (i + 1 < sorted.size()) {
        if (sorted[i + 1]->rating - sorted[i]->rating <= g_config.rating_tolerance) {
            matches.push_back({sorted[i], sorted[i + 1]});
            i += 2;
        } else {
            ++i;
        }
    }

    out->success = 1;
    out->error_code = "";
    out->error_msg = "";
    out->match_count = static_cast<int>(matches.size());

    if (matches.empty()) {
        out->matches = nullptr;
        return 0;
    }

    out->matches = static_cast<shield_matchmaking_match*>(
        std::calloc(matches.size(), sizeof(shield_matchmaking_match)));

    for (size_t m = 0; m < matches.size(); ++m) {
        auto& match = out->matches[m];
        std::string match_id = "match_" + std::to_string(g_next_match_id++);
        match.match_id = strdup(match_id.c_str());
        match.player_count = static_cast<int>(matches[m].size());
        match.players = static_cast<shield_matchmaking_player*>(
            std::calloc(matches[m].size(), sizeof(shield_matchmaking_player)));
        match.server_address = nullptr;

        for (size_t p = 0; p < matches[m].size(); ++p) {
            auto& player = match.players[p];
            player.player_id = strdup(matches[m][p]->player_id.c_str());
            player.rating = matches[m][p]->rating;
            player.party_id = nullptr;
            player.region = strdup(matches[m][p]->region.c_str());
            player.metadata_json = nullptr;
        }

        // Remove matched players from queue.
        for (auto* p : matches[m]) {
            g_queue.erase(p->player_id);
        }
    }
    return 0;
}

int mm_update_rating(shield_matchmaking_conn*, const char* player_id, double new_rating) {
    // In a real implementation, persist the rating.
    return 0;
}

int mm_get_rating(shield_matchmaking_conn*, const char* player_id, double* out_rating) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queue.find(player_id);
    if (it != g_queue.end()) {
        *out_rating = it->second.rating;
        return 0;
    }
    *out_rating = 0;
    return -1;
}

int mm_queue_size(shield_matchmaking_conn*, int* out_size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *out_size = static_cast<int>(g_queue.size());
    return 0;
}

void mm_free_result(shield_matchmaking_result* result) {
    if (!result || !result->matches) return;
    for (int i = 0; i < result->match_count; ++i) {
        auto& m = result->matches[i];
        if (m.match_id) std::free(const_cast<char*>(m.match_id));
        if (m.server_address) std::free(const_cast<char*>(m.server_address));
        if (m.players) {
            for (int j = 0; j < m.player_count; ++j) {
                if (m.players[j].player_id) std::free(const_cast<char*>(m.players[j].player_id));
                if (m.players[j].region) std::free(const_cast<char*>(m.players[j].region));
            }
            std::free(m.players);
        }
    }
    std::free(result->matches);
    result->matches = nullptr;
    result->match_count = 0;
}

const shield_matchmaking_plugin g_mm_plugin = {
    SHIELD_MATCHMAKING_ABI_VERSION,
    "elo",
    "1.0.0",

    nullptr, nullptr,  // connect/disconnect not needed

    mm_enqueue,
    mm_dequeue,
    mm_is_queued,
    mm_try_match,
    mm_update_rating,
    mm_get_rating,
    mm_queue_size,
    mm_free_result,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_MATCHMAKING,
    "shield_matchmaking_elo",
    "1.0.0",
    "ELO-based matchmaking plugin",
    "Shield",

    [](const shield_host_t, const shield_host_api*,
       const shield_plugin_config*, char*, int) -> int { return 0; },

    []() { mm_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"matchmaking", "1.0.0", "ELO matchmaking"};
        return &cap;
    },

    &g_mm_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
