#include "shield/lua/slow_calls.hpp"

#include <chrono>

namespace shield::lua {

SlowCallRing& SlowCallRing::instance() {
    static SlowCallRing ring;  // GCOVR_EXCL_BR_LINE (static init guard)
    return ring;
}

void SlowCallRing::enable(uint64_t threshold_ms) {
    // Sticky gate: 0 is the "off" sentinel and is ignored once anything
    // has armed the ring.
    if (threshold_ms == 0) {
        return;
    }
    threshold_ms_.store(threshold_ms, std::memory_order_relaxed);
}

void SlowCallRing::maybe_record(uint64_t begin_ms, std::string caller,
                                std::string callee, bool ok) {
    // One relaxed load for the common sub-threshold path: ordinary calls
    // pay an atomic read and nothing else.
    const uint64_t threshold = threshold_ms_.load(std::memory_order_relaxed);
    if (threshold == 0 || begin_ms == 0) {
        return;  // gate off, or the call predates the gate
    }
    // steady_clock is monotonic, so the span never goes negative.
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const uint64_t elapsed_ms = static_cast<uint64_t>(now) - begin_ms;
    if (elapsed_ms < threshold) {
        return;  // at-or-above records: == threshold counts as slow
    }
    SlowCallRecord record;
    record.caller = std::move(caller);
    record.callee = std::move(callee);
    record.elapsed_ms = elapsed_ms;
    record.ok = ok;
    record.at = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

    std::lock_guard lock(mutex_);
    slots_[next_] = std::move(record);
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) {
        ++count_;
    }
    ++total_recorded_;
}

SlowCallRing::Snapshot SlowCallRing::snapshot() const {
    Snapshot snap;
    std::lock_guard lock(mutex_);
    snap.total_recorded = total_recorded_;
    snap.recent.reserve(count_);
    // Newest first: walk backwards from the write cursor.
    for (std::size_t i = 0; i < count_; ++i) {
        snap.recent.push_back(slots_[(next_ + kCapacity - 1 - i) % kCapacity]);
    }
    return snap;
}

}  // namespace shield::lua
