// tools/bots/chaos.hpp - malformed-input injection against the gateway.
//
// Goal: discover gateway boundary bugs (crash / hang / connection leak /
// swallowed errors) with hostile clients. Closing the offending connection
// is EXPECTED behavior; the failure signal is the survival probe that runs
// after each injection failing — i.e. the gateway stopped serving normal
// traffic.
//
// Modes:
//   bad_frame         truncated header / garbage / lying length field
//   oversize          full-size 0xFFFF body declared and delivered
//   route_flood       unknown route ids + move-before-login ordering abuse
//   disconnect_flood  rapid connect/close cycles, half-open frames, RST
#ifndef SHIELD_TOOLS_BOTS_CHAOS_HPP
#define SHIELD_TOOLS_BOTS_CHAOS_HPP

#include <chrono>
#include <string>
#include <vector>

namespace shield::bots {

struct ChaosOptions {
    std::string target = "127.0.0.1:8001";
    int count = 8;                          // iterations per mode
    std::chrono::milliseconds settle{300};  // quiet wait before the probe
    std::chrono::milliseconds probe_timeout{2000};
    std::string player_prefix = "chaos";
};

struct ChaosReport {
    std::vector<std::string> lines;      // full human-readable transcript
    std::vector<std::string> anomalies;  // detail of every ANOMALY line

    bool clean() const { return anomalies.empty(); }
};

/// mode: all | bad_frame | oversize | route_flood | disconnect_flood.
/// Throws std::runtime_error on an unknown mode name.
ChaosReport run_chaos(const std::string& mode, const ChaosOptions& options);

/// Mode names accepted by run_chaos("all", ...) expansion order.
std::vector<std::string> chaos_mode_names();

}  // namespace shield::bots

#endif  // SHIELD_TOOLS_BOTS_CHAOS_HPP
