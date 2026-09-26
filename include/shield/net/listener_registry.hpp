// [SHIELD_NET] Process-wide registry of live TCP listeners.
//
// /ops/metrics needs a gateway view (accepts, accept-time rejections,
// rate-limited messages, active sessions), but listeners are owned by
// bootstrap-local state the ops handler cannot reach. Listeners register
// themselves here when they start listening and unregister on destruction;
// the registry holds raw non-owning pointers and never touches them.
#pragma once

#include <mutex>
#include <vector>

namespace shield::net {

class TcpListener;

class ListenerRegistry {
public:
    static ListenerRegistry& instance() {
        static ListenerRegistry registry;
        return registry;
    }

    void add(TcpListener* listener) {
        std::lock_guard lock(mutex_);
        listeners_.push_back(listener);
    }

    void remove(TcpListener* listener) {
        std::lock_guard lock(mutex_);
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (*it == listener) {
                listeners_.erase(it);
                return;
            }
        }
    }

    /// @brief Pointers of every live listener (any order).
    std::vector<TcpListener*> snapshot() const {
        std::lock_guard lock(mutex_);
        return listeners_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<TcpListener*> listeners_;
};

}  // namespace shield::net
