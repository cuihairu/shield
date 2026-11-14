// tools/bots/load_runner.hpp - plan execution: ramp + paced work + stats.
//
// One thread per connection (blocking client, no async machinery), spawned
// along the plan's ramp timeline; every thread stops at the same global
// deadline. Requests are strictly sequential per connection: send one work
// frame, await its reply (replies correlated by route id, wire order) —
// matching the gateway's fire-and-forget correlation contract.
#ifndef SHIELD_TOOLS_BOTS_LOAD_RUNNER_HPP
#define SHIELD_TOOLS_BOTS_LOAD_RUNNER_HPP

#include <string>

#include "metrics.hpp"
#include "plan.hpp"

namespace shield::bots {

struct LoadRunSummary {
    std::string plan_name;
    Metrics metrics;
    int connections_attempted = 0;
    int connections_failed = 0;  // never connected (backoff exhausted)
    int login_failures = 0;      // connected but login timed out / dropped
    int reconnects = 0;          // mid-run drops recovered by re-login
    double wall_seconds = 0.0;

    /// metrics.summary() + connection counters — the human block.
    std::string report() const;
};

/// Blocking; returns after the plan's global deadline plus join.
LoadRunSummary run_load(const LoadPlan& plan);

}  // namespace shield::bots

#endif  // SHIELD_TOOLS_BOTS_LOAD_RUNNER_HPP
