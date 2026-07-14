// [LUA]
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace shield::lua {

/// Abstract business-time clock.
///
/// Lua business code (shield.now / os.time / os.date without explicit time)
/// reads this clock. Production uses SystemClock (wall-clock UTC);
/// tests inject MockClock so that time can be advanced/set explicitly.
///
/// Design decision (AD-07): C++ infrastructure time — deadlines, message
/// timestamps, CAF delayed_send, pump scans — stays on real monotonic clock
/// and is NOT affected by this abstraction. Only the Lua-facing business-time
/// surface is pluggable.
class Clock {
public:
    virtual ~Clock() = default;

    /// Wall-clock UTC milliseconds. Used by shield.now() and as the basis
    /// for os.time() (seconds) / os.date() (no-arg).
    virtual int64_t now_ms() const = 0;

    /// Convenience: wall-clock UTC seconds (now_ms / 1000).
    int64_t now_seconds() const { return now_ms() / 1000; }
};

/// Production clock: reads std::chrono::system_clock (wall-clock UTC).
class SystemClock : public Clock {
public:
    int64_t now_ms() const override {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now)
            .count();
    }
};

/// Test clock: time is explicitly set/advanced. Reads are lock-free
/// (std::atomic), safe across multiple VMs / CAF actors.
/// Default-constructed with current real time — tests can immediately
/// use os.time()/shield.now() without an explicit set_now first.
class MockClock : public Clock {
public:
    MockClock() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        now_ms_.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count(),
            std::memory_order_relaxed);
    }

    int64_t now_ms() const override {
        return now_ms_.load(std::memory_order_relaxed);
    }

    /// Replace current mock time (absolute value).
    void set_now(int64_t ms) { now_ms_.store(ms, std::memory_order_relaxed); }

    /// Advance current mock time by delta_ms (relative).
    void advance(int64_t delta_ms) {
        now_ms_.fetch_add(delta_ms, std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> now_ms_{0};
};

}  // namespace shield::lua
