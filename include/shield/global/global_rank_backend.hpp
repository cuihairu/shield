// [SHIELD_GLOBAL] Rank-domain backend seam.
//
// The leaderboard domain sits behind this interface, mirroring
// GlobalDataBackend / GlobalLockBackend: GlobalManager owns a backend and
// delegates, keeping the public API on the manager. The in-process
// implementation is the P0 store; the redis++ implementation keeps each
// board as a ZSET (member = uid, score as-is) keyed
// `<prefix>:rank:<board>`. The board contract is (score desc, member
// asc on ties); note no ZSET traversal produces that order (descending
// traversal is the exact reverse of ascending, so ties come out member
// desc either way), so the redis++ implementation pulls the board and
// orders it with the in-process comparator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shield/global/global_data_backend.hpp"

namespace shield::global {

/// One leaderboard row. `rank` is the 1-based position inside the FULL
/// board order (score desc, uid asc on ties), not the position inside the
/// returned slice.
struct RankEntry {
    std::string uid;
    double score = 0.0;
    std::uint64_t rank = 0;  // 1-based
};

struct RankAround {
    std::vector<RankEntry> above;
    std::optional<RankEntry> target;
    std::vector<RankEntry> below;
};

class GlobalRankBackend {
public:
    virtual ~GlobalRankBackend() = default;

    virtual void rank_update(const std::string& board, const std::string& uid,
                             double score) = 0;
    virtual void rank_mupdate(
        const std::string& board,
        const std::vector<std::pair<std::string, double>>& updates) = 0;
    virtual std::optional<double> rank_score(const std::string& board,
                                             const std::string& uid) = 0;
    virtual std::optional<std::uint64_t> rank_position(
        const std::string& board, const std::string& uid) = 0;
    virtual std::vector<RankEntry> rank_top(const std::string& board,
                                            std::size_t n) = 0;
    /// 1-based inclusive rank window; out-of-range bounds clamp.
    virtual std::vector<RankEntry> rank_range(const std::string& board,
                                              std::uint64_t from,
                                              std::uint64_t to) = 0;
    /// Score window [lo, hi], rank order.
    virtual std::vector<RankEntry> rank_range_by_score(const std::string& board,
                                                       double lo,
                                                       double hi) = 0;
    virtual RankAround rank_around(const std::string& board,
                                   const std::string& uid, std::size_t n) = 0;
    virtual std::size_t rank_count(const std::string& board) = 0;
    virtual bool rank_remove(const std::string& board,
                             const std::string& uid) = 0;
    virtual void rank_clear(const std::string& board) = 0;
    virtual std::size_t rank_board_count() = 0;
    virtual std::size_t rank_total_members() = 0;
};

std::unique_ptr<GlobalRankBackend> make_inprocess_rank_backend();
std::unique_ptr<GlobalRankBackend> make_redis_rank_backend(
    const RedisDataBackendConfig& config);

}  // namespace shield::global
