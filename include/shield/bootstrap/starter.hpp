// [SHIELD_BOOTSTRAP] Starter system
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shield::bootstrap {

/// @brief Starter phase
enum class Phase {
    /// Before actor system starts
    PRE_INIT,

    /// After actor system starts, before services
    POST_SYSTEM_INIT,

    /// After config is loaded
    POST_CONFIG,

    /// After all services are started
    POST_START,

    /// Before shutdown
    PRE_SHUTDOWN,

    /// After all services stopped
    POST_SHUTDOWN
};

/// @brief Starter interface
class Starter {
public:
    virtual ~Starter() = default;

    /// @brief Get starter name
    virtual std::string name() const = 0;

    /// @brief Get execution order (lower = earlier)
    virtual int order() const { return 100; }

    /// @brief Execute starter in a phase
    /// @param phase The current phase
    /// @return true if successful
    virtual bool execute(Phase phase) = 0;

    /// @brief Check if this starter should run in the given phase
    virtual bool should_run(Phase phase) const { return true; }
};

/// @brief Register a starter
void register_starter(std::unique_ptr<Starter> starter);

/// @brief Run all starters for a phase
/// @return true if all starters succeeded
bool run_starters(Phase phase);

/// @brief Convenience: function-based starter
class FunctionStarter : public Starter {
public:
    FunctionStarter(std::string name,
                  int order,
                  std::function<bool(Phase)> fn)
        : name_(std::move(name)), order_(order), fn_(std::move(fn)) {}

    std::string name() const override { return name_; }
    int order() const override { return order_; }

    bool execute(Phase phase) override {
        return fn_(phase);
    }

private:
    std::string name_;
    int order_;
    std::function<bool(Phase)> fn_;
};

}  // namespace shield::bootstrap
