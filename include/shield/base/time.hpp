// [SHIELD_BASE] Time types for Shield runtime
#pragma once

#include <chrono>
#include <cstdint>

namespace shield::base {

/// @brief Duration type used internally for timeouts and timers
/// Using millisecond resolution for game server timing
using Duration = std::chrono::milliseconds;

/// @brief Time point type (mononic, not wall clock)
using TimePoint = std::chrono::steady_clock::time_point;

/// @brief Clock type (steady clock for reliable timing)
using Clock = std::chrono::steady_clock;

/// @brief Get current monotonic time
inline TimePoint now() { return Clock::now(); }

/// @brief Get current time in milliseconds since epoch
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief Convert duration to milliseconds
inline int64_t to_millis(Duration d) { return d.count(); }

/// @brief Convert milliseconds to duration
inline Duration from_millis(int64_t ms) { return Duration(ms); }

/// @brief Check if a timeout has expired
inline bool is_expired(TimePoint deadline) { return now() >= deadline; }

/// @brief Calculate remaining time until deadline
inline Duration remaining(TimePoint deadline) {
    auto remaining = deadline - now();
    if (remaining.count() < 0) {
        return Duration(0);
    }
    return std::chrono::duration_cast<Duration>(remaining);
}

}  // namespace shield::base
