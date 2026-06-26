// [SHIELD_BOOTSTRAP] Starter implementation
#include "shield/bootstrap/starter.hpp"

#include <algorithm>
#include <vector>

#include "shield/log/logger.hpp"

namespace shield::bootstrap {

// Global starter registry
static std::vector<std::unique_ptr<Starter>>& starters() {
    static std::vector<std::unique_ptr<Starter>> registry;
    return registry;
}

// Register a starter
void register_starter(std::unique_ptr<Starter> starter) {
    starters().push_back(std::move(starter));

    // Sort by order
    std::sort(
        starters().begin(), starters().end(),
        [](const auto& a, const auto& b) { return a->order() < b->order(); });
}

// Run all starters for a phase
bool run_starters(Phase phase) {
    auto& log = shield::log::get_logger("starter");

    std::string phase_name;
    switch (phase) {
        case Phase::PRE_INIT:
            phase_name = "PRE_INIT";
            break;
        case Phase::POST_SYSTEM_INIT:
            phase_name = "POST_SYSTEM_INIT";
            break;
        case Phase::POST_CONFIG:
            phase_name = "POST_CONFIG";
            break;
        case Phase::POST_START:
            phase_name = "POST_START";
            break;
        case Phase::PRE_SHUTDOWN:
            phase_name = "PRE_SHUTDOWN";
            break;
        case Phase::POST_SHUTDOWN:
            phase_name = "POST_SHUTDOWN";
            break;
    }

    SHIELD_LOG_INFO(log, "Running starters for phase: " + phase_name);

    for (const auto& starter : starters()) {
        if (!starter->should_run(phase)) {
            continue;
        }

        SHIELD_LOG_DEBUG(log, "Running starter: " + starter->name());

        if (!starter->execute(phase)) {
            SHIELD_LOG_ERROR(log, "Starter failed: " + starter->name());
            return false;
        }
    }

    SHIELD_LOG_INFO(log, "All starters completed for phase: " + phase_name);
    return true;
}

}  // namespace shield::bootstrap
