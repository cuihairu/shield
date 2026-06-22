// [SHIELD_PLUGIN] matchmaking.elo — ELO provider for shield.matchmaking.v1.
//
// Implements the v1 ABI (shield_plugin_get_v1). Each instance holds its own
// queue and config; the matchmaking connection handle IS the instance
// (shell-first layout), so connect/disconnect simply return/destroy self.
//
// Matching algorithm: greedy sort by rating, pair adjacent players within
// rating_tolerance. No persistence — queue lives in process memory.

#include "shield/plugin/abi.h"
#include "shield/plugin/matchmaking.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct PlayerEntry {
    std::string player_id;
    double rating = 0.0;
    std::string party_id;
    std::string region;
};

struct MatchConfig {
    int max_players_per_match = 2;
    int match_timeout_seconds = 30;
    double rating_tolerance = 200.0;
};

// shield_matchmaking_conn is forward-declared opaque in matchmaking.h. The
// concrete layout IS the instance (we expose the instance shell as the conn
// handle to vtable methods, then cast back).
struct shield_matchmaking_conn {
    // Must match matchmaking_instance layout: shell-first. We cast between
    // them via reinterpret_cast, so the layout here is for documentation.
};

struct matchmaking_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    MatchConfig cfg;
    std::unordered_map<std::string, PlayerEntry> queue;
    std::atomic<uint64_t> next_match_id{1};
    std::mutex mu;
};

char* dup_cstr(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// Parse instance config_json into MatchConfig. Tolerant: missing fields keep
// defaults.
MatchConfig parse_config(const char* config_json) {
    MatchConfig c;
    if (!config_json || !config_json[0]) return c;
    try {
        // Minimal hand-parsing to avoid pulling nlohmann here. The host already
        // validated against config_schema, so we only extract known keys.
        std::string s(config_json);
        auto find_int = [&](const std::string& key, int def) -> int {
            auto k = "\"" + key + "\"";
            auto p = s.find(k);
            if (p == std::string::npos) return def;
            p = s.find(':', p);
            if (p == std::string::npos) return def;
            ++p;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            try { return std::stoi(s.substr(p)); } catch (...) { return def; }
        };
        auto find_double = [&](const std::string& key, double def) -> double {
            auto k = "\"" + key + "\"";
            auto p = s.find(k);
            if (p == std::string::npos) return def;
            p = s.find(':', p);
            if (p == std::string::npos) return def;
            ++p;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            try { return std::stod(s.substr(p)); } catch (...) { return def; }
        };
        c.max_players_per_match = find_int("max_players_per_match", c.max_players_per_match);
        c.match_timeout_seconds = find_int("match_timeout_seconds", c.match_timeout_seconds);
        c.rating_tolerance = find_double("rating_tolerance", c.rating_tolerance);
    } catch (...) {}
    return c;
}

// ---------------------------------------------------------------------------
// v1 matchmaking vtable. All ops cast `conn` back to the instance.
// ---------------------------------------------------------------------------
const shield_matchmaking_v1& mm_vtable() {
    static const shield_matchmaking_v1 v = {
        sizeof(shield_matchmaking_v1),
        "elo",
        "1.0.0",
        // connect (returns instance as conn handle; instance already created)
        [](const struct shield_matchmaking_config* cfg,
           char* err_buf, int err_buf_size) -> struct shield_matchmaking_conn* {
            (void)cfg;
            if (err_buf && err_buf_size > 0) err_buf[0] = '\0';
            return nullptr;  // connect is a no-op for elo; instance owns state
        },
        // disconnect (no-op; instance lifecycle owned by PluginHost)
        [](struct shield_matchmaking_conn*) {},
        // enqueue
        [](struct shield_matchmaking_conn* conn,
           const struct shield_matchmaking_player* player) -> int {
            if (!conn || !player || !player->player_id) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            PlayerEntry e;
            e.player_id = player->player_id;
            e.rating = player->rating;
            e.party_id = player->party_id ? player->party_id : "";
            e.region = player->region ? player->region : "";
            inst->queue[player->player_id] = std::move(e);
            return 0;
        },
        // dequeue
        [](struct shield_matchmaking_conn* conn,
           const char* player_id) -> int {
            if (!conn || !player_id) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            return inst->queue.erase(player_id) > 0 ? 0 : -1;
        },
        // is_queued
        [](struct shield_matchmaking_conn* conn,
           const char* player_id) -> int {
            if (!conn || !player_id) return 0;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            return inst->queue.count(player_id) > 0 ? 1 : 0;
        },
        // try_match
        [](struct shield_matchmaking_conn* conn,
           struct shield_matchmaking_result* out) -> int {
            if (!conn || !out) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);

            std::vector<PlayerEntry*> sorted;
            sorted.reserve(inst->queue.size());
            for (auto& [id, p] : inst->queue) sorted.push_back(&p);
            std::sort(sorted.begin(), sorted.end(),
                      [](const PlayerEntry* a, const PlayerEntry* b) {
                          return a->rating < b->rating;
                      });

            // Greedy: group consecutive players within rating_tolerance up to
            // max_players_per_match.
            std::vector<std::vector<PlayerEntry*>> matches;
            size_t i = 0;
            while (i < sorted.size()) {
                std::vector<PlayerEntry*> group;
                group.push_back(sorted[i]);
                size_t j = i + 1;
                while (j < sorted.size() &&
                       static_cast<int>(group.size()) < inst->cfg.max_players_per_match &&
                       std::abs(sorted[j]->rating - group.front()->rating) <= inst->cfg.rating_tolerance) {
                    group.push_back(sorted[j]);
                    ++j;
                }
                if (static_cast<int>(group.size()) == inst->cfg.max_players_per_match) {
                    matches.push_back(std::move(group));
                    i = j;
                } else {
                    ++i;
                }
            }

            out->success = 1;
            out->error_code = nullptr;
            out->error_msg = nullptr;
            out->match_count = static_cast<int>(matches.size());
            out->matches = nullptr;
            if (matches.empty()) return 0;

            out->matches = static_cast<struct shield_matchmaking_match*>(
                std::calloc(matches.size(), sizeof(struct shield_matchmaking_match)));
            if (!out->matches) {
                out->match_count = 0;
                return -1;
            }

            for (size_t m = 0; m < matches.size(); ++m) {
                auto& match = out->matches[m];
                std::string mid = "match_" + std::to_string(inst->next_match_id++);
                match.match_id = dup_cstr(mid.c_str());
                match.player_count = static_cast<int>(matches[m].size());
                match.players = static_cast<struct shield_matchmaking_player*>(
                    std::calloc(matches[m].size(), sizeof(struct shield_matchmaking_player)));
                match.server_address = nullptr;
                for (size_t p = 0; p < matches[m].size(); ++p) {
                    auto& pl = match.players[p];
                    pl.player_id = dup_cstr(matches[m][p]->player_id.c_str());
                    pl.rating = matches[m][p]->rating;
                    pl.party_id = dup_cstr(matches[m][p]->party_id);
                    pl.region = dup_cstr(matches[m][p]->region.c_str());
                    pl.metadata_json = nullptr;
                }
                // Remove matched players from the queue.
                for (auto* p : matches[m]) inst->queue.erase(p->player_id);
            }
            return 0;
        },
        // update_rating (no persistence; future hook for storage backends)
        [](struct shield_matchmaking_conn* conn,
           const char* player_id, double new_rating) -> int {
            if (!conn || !player_id) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            auto it = inst->queue.find(player_id);
            if (it == inst->queue.end()) return -1;
            it->second.rating = new_rating;
            return 0;
        },
        // get_rating
        [](struct shield_matchmaking_conn* conn,
           const char* player_id, double* out_rating) -> int {
            if (!conn || !player_id || !out_rating) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            auto it = inst->queue.find(player_id);
            if (it == inst->queue.end()) {
                *out_rating = 0;
                return -1;
            }
            *out_rating = it->second.rating;
            return 0;
        },
        // queue_size
        [](struct shield_matchmaking_conn* conn, int* out_size) -> int {
            if (!conn || !out_size) return -1;
            auto* inst = reinterpret_cast<matchmaking_instance*>(conn);
            std::lock_guard<std::mutex> lock(inst->mu);
            *out_size = static_cast<int>(inst->queue.size());
            return 0;
        },
        // free_result
        [](struct shield_matchmaking_result* result) {
            if (!result || !result->matches) return;
            for (int i = 0; i < result->match_count; ++i) {
                auto& m = result->matches[i];
                if (m.match_id) std::free(const_cast<char*>(m.match_id));
                if (m.server_address) std::free(const_cast<char*>(m.server_address));
                if (m.players) {
                    for (int j = 0; j < m.player_count; ++j) {
                        if (m.players[j].player_id) std::free(const_cast<char*>(m.players[j].player_id));
                        if (m.players[j].party_id) std::free(const_cast<char*>(m.players[j].party_id));
                        if (m.players[j].region) std::free(const_cast<char*>(m.players[j].region));
                        if (m.players[j].metadata_json) std::free(const_cast<char*>(m.players[j].metadata_json));
                    }
                    std::free(m.players);
                }
            }
            std::free(result->matches);
            result->matches = nullptr;
            result->match_count = 0;
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// v1 ABI entry: instance factory + shell.
// ---------------------------------------------------------------------------
int mm_create(const struct shield_plugin_create_args_v1* args,
              struct shield_plugin_instance_v1** out,
              struct shield_error_v1* err) {
    if (!args || !out) return 1;
    auto* inst = new (std::nothrow) matchmaking_instance;
    if (!inst) {
        if (err) {
            err->code = "plugin.create.failed";
            err->message = "matchmaking.elo: out of memory";
        }
        return 1;
    }
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->cfg = parse_config(args->config_json);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (!self || !iface) return nullptr;
        if (std::strcmp(iface, SHIELD_MATCHMAKING_INTERFACE) == 0) {
            return &mm_vtable();
        }
        return nullptr;
    };
    inst->shell.start = [](struct shield_plugin_instance_v1*, struct shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        delete reinterpret_cast<matchmaking_instance*>(self);
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
        "matchmaking.elo",
        "1.0.0",
        mm_create,
    };
    return &abi;
}
