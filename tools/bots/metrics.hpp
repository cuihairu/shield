// tools/bots/metrics.hpp - per-route latency/counters + percentile report.
//
// One instance per bot thread (not shared); the load runner merges the
// per-thread instances at join via merge().
#ifndef SHIELD_TOOLS_BOTS_METRICS_HPP
#define SHIELD_TOOLS_BOTS_METRICS_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace shield::bots {

struct RouteStats {
    std::uint64_t sent = 0;          // frames written to the socket
    std::uint64_t send_failed = 0;   // write errors (dead connection)
    std::uint64_t replies = 0;       // matching reply frames seen
    std::uint64_t timeouts = 0;      // sent but no reply within timeout_ms
    std::vector<double> latency_ms;  // reply latency samples

    void merge(const RouteStats& other) {
        sent += other.sent;
        send_failed += other.send_failed;
        replies += other.replies;
        timeouts += other.timeouts;
        latency_ms.insert(latency_ms.end(), other.latency_ms.begin(),
                          other.latency_ms.end());
    }
};

/// Keyed by route_id; s2c replies land on their own route id, so login
/// (1->100) and work (2->200) each get their natural bucket.
class Metrics {
public:
    RouteStats& route(std::uint32_t route_id) { return routes_[route_id]; }

    void merge(const Metrics& other) {
        for (const auto& [route_id, stats] : other.routes_) {
            routes_[route_id].merge(stats);
        }
    }

    /// "P50=1.2ms P95=3.4ms P99=5.6ms" over the samples, or "n/a" when none.
    static std::string percentiles(const std::vector<double>& samples) {
        if (samples.empty()) {
            return "n/a";
        }
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](double fraction) {
            auto index = static_cast<std::size_t>(
                fraction * static_cast<double>(sorted.size()));
            if (index >= sorted.size()) {
                index = sorted.size() - 1;
            }
            return sorted[index];
        };
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer),
                      "P50=%.2fms P95=%.2fms P99=%.2fms max=%.2fms n=%zu",
                      at(0.50), at(0.95), at(0.99), sorted.back(),
                      sorted.size());
        return std::string(buffer);
    }

    /// Multi-line human summary, one line per route plus a totals line.
    std::string summary() const {
        std::string out;
        char line[160];
        std::uint64_t total_sent = 0;
        std::uint64_t total_failed = 0;
        std::uint64_t total_replies = 0;
        std::uint64_t total_timeouts = 0;
        std::vector<double> all;
        for (const auto& [route_id, stats] : routes_) {
            total_sent += stats.sent;
            total_failed += stats.send_failed;
            total_replies += stats.replies;
            total_timeouts += stats.timeouts;
            all.insert(all.end(), stats.latency_ms.begin(),
                       stats.latency_ms.end());
            std::snprintf(line, sizeof(line),
                          "route %u: sent=%llu ok=%llu timeout=%llu "
                          "send_fail=%llu %s\n",
                          route_id, static_cast<unsigned long long>(stats.sent),
                          static_cast<unsigned long long>(stats.replies),
                          static_cast<unsigned long long>(stats.timeouts),
                          static_cast<unsigned long long>(stats.send_failed),
                          percentiles(stats.latency_ms).c_str());
            out += line;
        }
        std::snprintf(line, sizeof(line),
                      "total: sent=%llu ok=%llu timeout=%llu send_fail=%llu "
                      "%s\n",
                      static_cast<unsigned long long>(total_sent),
                      static_cast<unsigned long long>(total_replies),
                      static_cast<unsigned long long>(total_timeouts),
                      static_cast<unsigned long long>(total_failed),
                      percentiles(all).c_str());
        out += line;
        return out;
    }

    const std::map<std::uint32_t, RouteStats>& routes() const {
        return routes_;
    }

private:
    std::map<std::uint32_t, RouteStats> routes_;
};

}  // namespace shield::bots

#endif  // SHIELD_TOOLS_BOTS_METRICS_HPP
