// tools/bots/plan.hpp - load plan model + YAML parsing.
//
// Plan files are YAML (yaml-cpp ships with the repo's dependency set; a
// hand-rolled TOML subset would repeat the hand-written-JSON escaping trap).
// Schema (all fields optional unless noted):
//   target: "127.0.0.1:8001"        # required
//   name: hall-load
//   duration_seconds: 10
//   connections: 50                 # flat count, or ramp list below
//   ramp:                           # staged connection counts
//     - {at_second: 0, connections: 10}
//     - {at_second: 5, connections: 50}
//   requests_per_second: 5          # per connection, work phase
//   timeout_ms: 2000
//   reconnect_max_attempts: 3
//   login: {route_id: 1, reply_route_id: 100, body: {player_id: "bot"}}
//   work_routes:                    # required; weighted c2s mix
//     - route_id: 2
//       weight: 9
//       reply_route_id: 200
//       body: {x: 10, y: 20}
#ifndef SHIELD_TOOLS_BOTS_PLAN_HPP
#define SHIELD_TOOLS_BOTS_PLAN_HPP

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace shield::bots {

struct WorkRoute {
    std::uint32_t route_id = 0;
    std::uint32_t reply_route_id = 0;
    int weight = 1;
    nlohmann::json body;
};

struct RampStage {
    double at_second = 0.0;
    int connections = 0;
};

struct LoadPlan {
    std::string name = "bots-load";
    std::string target;
    double duration_seconds = 10.0;
    int connections = 10;
    std::vector<RampStage> ramp;  // overrides `connections` when non-empty
    double requests_per_second = 5.0;
    int timeout_ms = 2000;
    int reconnect_max_attempts = 3;
    std::string login_player_prefix = "bot";
    std::uint32_t login_route_id = 1;
    std::uint32_t login_reply_route_id = 100;
    std::vector<WorkRoute> work_routes;

    /// Connections live at second t (ramp interpolation by stage boundary).
    int connections_at(double second) const;
    /// Total distinct connections the plan will create.
    int peak_connections() const;
};

/// Parse and validate a plan file. Throws std::runtime_error with a
/// human-readable message on any schema problem.
LoadPlan load_plan(const std::string& path);

}  // namespace shield::bots

#endif  // SHIELD_TOOLS_BOTS_PLAN_HPP
