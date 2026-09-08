#define BOOST_TEST_MODULE CovStarter
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>
#include <vector>

#include "shield/bootstrap/starter.hpp"

using shield::bootstrap::FunctionStarter;
using shield::bootstrap::Phase;
using shield::bootstrap::register_starter;
using shield::bootstrap::run_starters;
using shield::bootstrap::Starter;

namespace {

std::vector<std::string>& trace() {
    static std::vector<std::string> v;
    return v;
}

// Starter that only runs in a single phase (covers should_run == false).
class SinglePhaseStarter : public shield::bootstrap::Starter {
public:
    SinglePhaseStarter(std::string name, Phase only)
        : name_(std::move(name)), only_(only) {}

    std::string name() const override { return name_; }
    int order() const override { return 5; }
    bool should_run(Phase phase) const override { return phase == only_; }
    bool execute(Phase phase) override {
        trace().push_back(name_ + "@exec");
        return true;
    }

private:
    std::string name_;
    Phase only_;
};

}  // namespace

BOOST_AUTO_TEST_CASE(RegisterSortsAndRunsPerPhase) {
    trace().clear();

    register_starter(
        std::make_unique<FunctionStarter>("late", 90, [](Phase phase) {
            trace().push_back("late:" +
                              std::to_string(static_cast<int>(phase)));
            return true;
        }));
    register_starter(
        std::make_unique<FunctionStarter>("early", 10, [](Phase phase) {
            trace().push_back("early:" +
                              std::to_string(static_cast<int>(phase)));
            return true;
        }));
    register_starter(
        std::make_unique<SinglePhaseStarter>("only_pre_init", Phase::PRE_INIT));

    // Run every phase so each switch label in run_starters is executed.
    const Phase phases[] = {Phase::PRE_INIT,     Phase::POST_SYSTEM_INIT,
                            Phase::POST_CONFIG,  Phase::POST_START,
                            Phase::PRE_SHUTDOWN, Phase::POST_SHUTDOWN};
    for (auto phase : phases) {
        BOOST_CHECK(run_starters(phase));
    }

    // "only_pre_init" executed exactly once (should_run filtering worked).
    size_t pre_execs = 0;
    for (const auto& entry : trace()) {
        if (entry == "only_pre_init@exec") ++pre_execs;
    }
    BOOST_CHECK_EQUAL(pre_execs, 1u);

    // Ordering: early (order 10) precedes late (order 90) per phase and
    // only_pre_init (order 5) precedes early in PRE_INIT.
    const std::string pre0 =
        "early:" + std::to_string(static_cast<int>(Phase::PRE_INIT));
    const std::string late0 =
        "late:" + std::to_string(static_cast<int>(Phase::PRE_INIT));
    auto pre_pos = std::find(trace().begin(), trace().end(), pre0);
    auto late_pos = std::find(trace().begin(), trace().end(), late0);
    BOOST_REQUIRE(pre_pos != trace().end());
    BOOST_REQUIRE(late_pos != trace().end());
    BOOST_CHECK(pre_pos < late_pos);
    auto only_pos =
        std::find(trace().begin(), trace().end(), "only_pre_init@exec");
    BOOST_REQUIRE(only_pos != trace().end());
    BOOST_CHECK(only_pos < pre_pos);
}

BOOST_AUTO_TEST_CASE(FailingStarterAbortsPhase) {
    // A starter that fails on PRE_SHUTDOWN makes run_starters return false.
    register_starter(std::make_unique<FunctionStarter>(
        "failing", 200,
        [](Phase phase) { return phase != Phase::PRE_SHUTDOWN; }));

    // Phases where the starter succeeds still pass.
    BOOST_CHECK(shield::bootstrap::run_starters(Phase::POST_CONFIG));
    // The failing phase aborts.
    BOOST_CHECK(!shield::bootstrap::run_starters(Phase::PRE_SHUTDOWN));
    // Other phases still work afterwards.
    BOOST_CHECK(shield::bootstrap::run_starters(Phase::POST_SHUTDOWN));
}

namespace {
// A starter that keeps the interface defaults for order() and should_run():
// exercises the base-class implementations in starter.hpp.
class DefaultPolicyStarter : public Starter {
public:
    explicit DefaultPolicyStarter(std::string name) : name_(std::move(name)) {}

    std::string name() const override { return name_; }
    bool execute(Phase) override { return true; }

private:
    std::string name_;
};
}  // namespace

BOOST_AUTO_TEST_CASE(StarterDefaultsOrderAndShouldRun) {
    DefaultPolicyStarter starter("defaults");
    BOOST_CHECK_EQUAL(starter.order(), 100);
    // The default should_run() accepts every phase.
    for (Phase phase :
         {Phase::PRE_INIT, Phase::POST_SYSTEM_INIT, Phase::POST_CONFIG,
          Phase::POST_START, Phase::PRE_SHUTDOWN, Phase::POST_SHUTDOWN}) {
        BOOST_CHECK(starter.should_run(phase));
    }
}
