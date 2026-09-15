// [SHIELD_CONSOLE] Shared global-capability snapshot builder (P0). See
// global_status.hpp.
#include "global_status.hpp"

#ifdef SHIELD_ENABLE_GLOBAL

#include "shield/global/global_manager.hpp"

namespace shield::console {

nlohmann::json build_global_status_json() {
    auto* gm = shield::global::GlobalManager::global();
    if (gm == nullptr) {
        return nullptr;
    }
    nlohmann::json root;
    root["data"] = {{"keys", gm->data_size()}};
    const std::uint64_t hits = gm->cache_hits();
    const std::uint64_t misses = gm->cache_misses();
    const std::uint64_t lookups = hits + misses;
    root["cache"] = {
        {"size", gm->cache_size()},
        {"hits", hits},
        {"misses", misses},
        {"hit_rate", lookups == 0 ? 0.0
                                  : static_cast<double>(hits) /
                                        static_cast<double>(lookups)},
    };
    root["locks"] = {
        {"mutexes", gm->mutex_registry_size("mutex")},
        {"spinlocks", gm->mutex_registry_size("spinlock")},
        {"rwlocks", gm->rwlock_count()},
    };
    root["ranks"] = {{"count", gm->rank_board_count()},
                     {"total_members", gm->rank_total_members()}};
    root["queues"] = {{"normal", gm->queue_count()},
                      {"delay", gm->delay_queue_count()},
                      {"priority", gm->priority_queue_count()},
                      {"broadcast", gm->broadcast_queue_count()},
                      {"reliable", gm->reliable_queue_count()}};
    std::size_t tasks = 0;
    for (const auto& task : gm->sched_list()) {
        (void)task;
        ++tasks;
    }
    root["scheduler"] = {{"tasks", tasks},
                         {"active", gm->sched_active_count()}};
    return root;
}

}  // namespace shield::console

#endif  // SHIELD_ENABLE_GLOBAL
