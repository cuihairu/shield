// [SHIELD_LUA] slow-call ring — bounded record of completed slow calls
//
// Phase B of the /ops/profile plan (docs/superpowers/plans/
// 2026-09-22-ops-profile-v1.md): while the sampling session answers
// "where is time spent inside a service", the slow-call ring answers
// "which cross-service calls were slow" — a completed shield.call span
// measured from suspend to resume, capped to the most recent samples.
//
// Pure storage: no Lua state, no CAF, no I/O. The call hot path touches
// only the sticky gate (one relaxed load) until a sample actually lands.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace shield::lua {

/// @brief One completed call that measured at or above the threshold.
struct SlowCallRecord {
    std::string caller;       ///< calling service id
    std::string callee;       ///< invoked service (or spawned module)
    uint64_t elapsed_ms = 0;  ///< suspend->resume span
    bool ok = false;          ///< call completed successfully
    int64_t at = 0;           ///< wall-clock unix seconds at recording
};

/// @brief Process-wide slow-call tracker.
///
/// The gate is sticky by design: the ops endpoint arms it once at
/// construction (non-zero http.slow_call_threshold_ms), and it never
/// resets within the process lifetime. A gate that could flap would
/// split call pairs — a call suspended while armed whose completion
/// lands after a reset would have nowhere to report — and re-arming
/// mid-stream would produce half-metered samples. Threshold updates
/// stay possible; disabling is not.
class SlowCallRing {
public:
    /// @brief Most recent slow calls, newest first.
    struct Snapshot {
        std::vector<SlowCallRecord> recent;
        uint64_t total_recorded = 0;  ///< cumulative, survives ring wrap
    };

    static constexpr std::size_t kCapacity = 64;

    static SlowCallRing& instance();

    /// @brief Arm the gate (sticky). Zero is ignored: 0 is the "off"
    /// sentinel, and the gate never turns back off once set.
    void enable(uint64_t threshold_ms);

    /// @brief Hot-path check: one relaxed load. Begin stamps are only
    /// taken while this is true.
    bool enabled() const noexcept {
        return threshold_ms_.load(std::memory_order_relaxed) != 0;
    }

    /// @brief Record a completed call that measured at or above the
    /// threshold. begin_ms == 0 (the call was suspended before the gate
    /// went up) and sub-threshold spans drop without touching the lock.
    void maybe_record(uint64_t begin_ms, std::string caller, std::string callee,
                      bool ok);

    /// @brief Newest-first copy of the ring plus the cumulative count.
    Snapshot snapshot() const;

    // Constructible: pure-logic suites drive stack-local instances; the
    // process-wide one is reached through instance() (same shape as
    // BodyCodecRegistry — singleton-ness lives in the accessor, not the
    // type).

    std::atomic<uint64_t> threshold_ms_{0};  // 0 = gate off
    mutable std::mutex mutex_;
    SlowCallRecord slots_[kCapacity];
    std::size_t next_ = 0;   ///< next slot to overwrite
    std::size_t count_ = 0;  ///< valid slots (<= kCapacity)
    uint64_t total_recorded_ = 0;
};

}  // namespace shield::lua
