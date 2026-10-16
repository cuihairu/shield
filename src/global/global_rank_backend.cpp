// GlobalRankBackend: the leaderboard domain behind one seam, mirroring the
// data and lock domains. The in-process implementation is the P0 store
// (member -> score maps, ordering computed by order_entries); the redis++
// implementation keeps each board as a ZSET (member = uid, score as-is)
// keyed <prefix>:rank:<board>. ZSET traversal cannot produce the board
// contract order (score desc, member asc on ties) in either direction, so
// every query pulls the board and orders it in C++ with the in-process
// comparator; only writes and counts are plain ZSET commands, and no Lua
// is needed.
#include "shield/global/global_rank_backend.hpp"

#include <sw/redis++/redis.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace shield::global {

namespace {

using Board = std::unordered_map<std::string, double>;

/// Contract order in place: score desc, uid asc on ties; ranks 1-based.
void order_entries(std::vector<RankEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const RankEntry& a, const RankEntry& b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  return a.uid < b.uid;
              });
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].rank = i + 1;
    }
}

/// Score desc, uid asc on ties; ranks are 1-based global positions.
std::vector<RankEntry> sorted_board(const Board& board) {
    std::vector<RankEntry> entries;
    entries.reserve(board.size());
    for (const auto& [uid, score] : board) {
        entries.push_back(RankEntry{// GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // aggregate init-list branches)
                                    uid, score, 0});
    }
    order_entries(entries);
    return entries;
}  // GCOVR_EXCL_LINE (gcov epilogue artifact: callers inline sorted_board,
   // the outlined epilogue never runs)

class InProcessRankBackend final : public GlobalRankBackend {
public:
    void rank_update(const std::string& board, const std::string& uid,
                     double score) override {
        std::lock_guard<std::mutex> lock(mu_);
        boards_[board][uid] = score;
    }

    void rank_mupdate(
        const std::string& board,
        const std::vector<std::pair<std::string, double>>& updates) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto& target = boards_[board];
        for (const auto& [uid, score] : updates) {
            target[uid] = score;
        }
    }

    std::optional<double> rank_score(const std::string& board,
                                     const std::string& uid) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end()) {
            return std::nullopt;
        }
        auto entry = it->second.find(uid);
        if (entry == it->second.end()) {
            return std::nullopt;
        }
        return entry->second;
    }

    std::optional<std::uint64_t> rank_position(
        const std::string& board, const std::string& uid) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end()) {
            return std::nullopt;
        }
        const auto sorted = sorted_board(it->second);
        for (const auto& entry : sorted) {
            if (entry.uid == uid) {
                return entry.rank;
            }
        }
        return std::nullopt;
    }

    std::vector<RankEntry> rank_top(const std::string& board,
                                    std::size_t n) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end() || n == 0) {
            return {};
        }
        auto sorted = sorted_board(it->second);
        if (sorted.size() > n) {
            sorted.resize(n);
        }
        return sorted;
    }

    std::vector<RankEntry> rank_range(const std::string& board,
                                      std::uint64_t from,
                                      std::uint64_t to) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end() || from == 0 || to < from) {
            return {};
        }
        const auto sorted = sorted_board(it->second);
        const std::size_t begin = std::min<std::size_t>(
            static_cast<std::size_t>(from - 1), sorted.size());
        const std::size_t end =
            std::min<std::size_t>(static_cast<std::size_t>(to), sorted.size());
        return std::vector<RankEntry>(  // GCOVR_EXCL_LINE (compiler artifact:
                                        // the split return credits the next
                                        // physical line)
            sorted.begin() + begin, sorted.begin() + end);
    }

    std::vector<RankEntry> rank_range_by_score(const std::string& board,
                                               double lo, double hi) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end() || hi < lo) {
            return {};
        }
        std::vector<RankEntry> out;
        for (const auto& entry : sorted_board(it->second)) {
            if (entry.score >= lo && entry.score <= hi) {
                out.push_back(entry);
            }
        }
        return out;
    }

    RankAround rank_around(const std::string& board, const std::string& uid,
                           std::size_t n) override {
        std::lock_guard<std::mutex> lock(mu_);
        RankAround around;
        auto it = boards_.find(board);
        if (it == boards_.end()) {
            return around;
        }
        const auto sorted = sorted_board(it->second);
        std::size_t pos = sorted.size();
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i].uid == uid) {
                pos = i;
                break;
            }
        }
        if (pos == sorted.size()) {
            return around;
        }
        around.target = sorted[pos];
        const std::size_t first = pos > n ? pos - n : 0;
        around.above.assign(sorted.begin() + first, sorted.begin() + pos);
        around.below.assign(
            sorted.begin() + pos + 1,
            sorted.begin() + std::min(pos + 1 + n, sorted.size()));
        return around;
    }

    std::size_t rank_count(const std::string& board) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        return it == boards_.end() ? 0 : it->second.size();
    }

    bool rank_remove(const std::string& board,
                     const std::string& uid) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it == boards_.end()) {
            return false;
        }
        return it->second.erase(uid) != 0;
    }

    void rank_clear(const std::string& board) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = boards_.find(board);
        if (it != boards_.end()) {
            it->second.clear();
        }
    }

    std::size_t rank_board_count() override {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t non_empty = 0;
        for (const auto& [board, entries] : boards_) {
            (void)board;
            if (!entries.empty()) {
                ++non_empty;
            }
        }
        return non_empty;
    }

    std::size_t rank_total_members() override {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t total = 0;
        for (const auto& [board, entries] : boards_) {
            (void)board;
            total += entries.size();
        }
        return total;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, Board> boards_;
};

class RedisRankBackend final : public GlobalRankBackend {
public:
    explicit RedisRankBackend(RedisDataBackendConfig config)
        : cfg_(std::move(config)) {}

    void rank_update(const std::string& board, const std::string& uid,
                     double score) override {
        auto redis = connect();
        redis->zadd(prefixed_board(board), uid, score);
    }

    void rank_mupdate(
        const std::string& board,
        const std::vector<std::pair<std::string, double>>& updates) override {
        auto redis = connect();
        const std::string key = prefixed_board(board);
        // One ZADD per member: the contract does not promise cross-member
        // atomicity, so no Lua wrapper is worth the payload.
        for (const auto& [uid, score] : updates) {
            redis->zadd(key, uid, score);
        }
    }

    std::optional<double> rank_score(const std::string& board,
                                     const std::string& uid) override {
        auto redis = connect();
        // redis++ (vcpkg build) hands back its own Optional<double>, which
        // mirrors std::optional one-to-one but needs an explicit lift.
        const auto score = redis->zscore(prefixed_board(board), uid);
        if (!score) {
            return std::nullopt;
        }
        return static_cast<double>(*score);
    }

    std::optional<std::uint64_t> rank_position(
        const std::string& board, const std::string& uid) override {
        auto redis = connect();
        const auto sorted = sorted_board(redis, board);
        for (const auto& entry : sorted) {
            if (entry.uid == uid) {
                return entry.rank;
            }
        }
        return std::nullopt;
    }

    std::vector<RankEntry> rank_top(const std::string& board,
                                    std::size_t n) override {
        auto redis = connect();
        auto sorted = sorted_board(redis, board);
        if (sorted.size() > n) {
            sorted.resize(n);
        }
        return sorted;
    }

    std::vector<RankEntry> rank_range(const std::string& board,
                                      std::uint64_t from,
                                      std::uint64_t to) override {
        std::vector<RankEntry> out;
        if (from == 0 || to < from) {
            return out;
        }
        auto redis = connect();
        const auto sorted = sorted_board(redis, board);
        const std::size_t begin = std::min<std::size_t>(
            static_cast<std::size_t>(from - 1), sorted.size());
        const std::size_t end =
            std::min<std::size_t>(static_cast<std::size_t>(to), sorted.size());
        out.assign(sorted.begin() + begin, sorted.begin() + end);
        return out;
    }

    std::vector<RankEntry> rank_range_by_score(const std::string& board,
                                               double lo, double hi) override {
        std::vector<RankEntry> out;
        if (hi < lo) {
            return out;
        }
        // Pull the whole board in rank order, then filter: ranks stay the
        // global board positions exactly like the in-process implementation.
        auto redis = connect();
        for (const auto& entry : sorted_board(redis, board)) {
            if (entry.score >= lo && entry.score <= hi) {
                out.push_back(entry);
            }
        }
        return out;
    }

    RankAround rank_around(const std::string& board, const std::string& uid,
                           std::size_t n) override {
        auto redis = connect();
        RankAround around;
        const auto sorted = sorted_board(redis, board);
        std::size_t pos = sorted.size();
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i].uid == uid) {
                pos = i;
                break;
            }
        }
        if (pos == sorted.size()) {
            return around;
        }
        around.target = sorted[pos];
        const std::size_t first = pos > n ? pos - n : 0;
        around.above.assign(sorted.begin() + first, sorted.begin() + pos);
        around.below.assign(
            sorted.begin() + pos + 1,
            sorted.begin() + std::min(pos + 1 + n, sorted.size()));
        return around;
    }

    std::size_t rank_count(const std::string& board) override {
        auto redis = connect();
        return static_cast<std::size_t>(redis->zcard(prefixed_board(board)));
    }

    bool rank_remove(const std::string& board,
                     const std::string& uid) override {
        auto redis = connect();
        return redis->zrem(prefixed_board(board), uid) != 0;
    }

    void rank_clear(const std::string& board) override {
        auto redis = connect();
        redis->del(prefixed_board(board));
    }

    std::size_t rank_board_count() override {
        // Every key is a non-empty board: Redis deletes an empty zset, so the
        // key count is the non-empty board count.
        return scan_boards().size();
    }

    std::size_t rank_total_members() override {
        auto redis = connect();
        std::size_t total = 0;
        for (const auto& key : scan_boards()) {
            total += static_cast<std::size_t>(redis->zcard(key));
        }
        return total;
    }

private:
    std::string prefixed_board(const std::string& board) const {
        return cfg_.prefix + ":rank:" + board;
    }

    /// Full board in contract order (score desc, member asc on ties).
    /// No ZSET traversal produces this order: ascending traversal gives
    /// (score asc, member asc) and descending traversal is its exact
    /// reverse, so ties come out member-desc either way. The board is
    /// pulled in one round trip and ordered by the same comparator as the
    /// in-process implementation — the same cost class as its
    /// sort-every-query queries.
    std::vector<RankEntry> sorted_board(
        const std::shared_ptr<sw::redis::Redis>& redis,
        const std::string& board) const {
        const std::string key = prefixed_board(board);
        std::vector<std::pair<std::string, double>> rows;
        redis->zrange(key, 0, -1, std::back_inserter(rows));
        std::vector<RankEntry> out;
        out.reserve(rows.size());
        for (auto& row : rows) {
            out.push_back(RankEntry{// GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // aggregate init-list branches)
                                    std::move(row.first), row.second, 0});
        }
        order_entries(out);
        return out;
    }

    std::vector<std::string> scan_boards() {
        auto redis = connect();
        std::vector<std::string> keys;
        const std::string pattern = cfg_.prefix + ":rank:*";
        long long cursor = 0;
        do {
            cursor =
                redis->scan(cursor, pattern, 100, std::back_inserter(keys));
        } while (cursor != 0);
        return keys;
    }

    // Lazy connect: construction must not block, so the Redis handle is
    // built on first use (same rule as the data and lock backends).
    std::shared_ptr<sw::redis::Redis> connect() {
        std::lock_guard<std::mutex> lock(mu_);
        if (redis_) return redis_;
        sw::redis::ConnectionOptions opts;
        opts.host = cfg_.host;
        opts.port = cfg_.port > 0 ? cfg_.port : 6379;
        if (!cfg_.password.empty()) opts.password = cfg_.password;
        opts.db = cfg_.db > 0 ? cfg_.db : 0;
        opts.connect_timeout = 2000ms;
        opts.socket_timeout = 2000ms;
        redis_ = std::make_shared<sw::redis::Redis>(opts);
        return redis_;
    }

    RedisDataBackendConfig cfg_;
    std::mutex mu_;
    std::shared_ptr<sw::redis::Redis> redis_;
};

}  // namespace

std::unique_ptr<GlobalRankBackend> make_inprocess_rank_backend() {
    return std::make_unique<InProcessRankBackend>();
}

std::unique_ptr<GlobalRankBackend> make_redis_rank_backend(
    const RedisDataBackendConfig& config) {
    return std::make_unique<RedisRankBackend>(config);
}

}  // namespace shield::global
