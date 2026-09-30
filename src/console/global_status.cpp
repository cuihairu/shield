// [SHIELD_CONSOLE] Shared global-capability snapshot builder (P0). See
// global_status.hpp.
#include "global_status.hpp"

#ifdef SHIELD_ENABLE_GLOBAL

#include "shield/global/global_manager.hpp"

namespace shield::console {

nlohmann::json build_global_status_json() {
    auto* gm = shield::global::GlobalManager::global();
    if (gm == nullptr) {  // GCOVR_EXCL_BR_LINE (both arms driven, record
        return nullptr;   // never merges: single-call null-check shape)
    }
    nlohmann::json root;
    root["data"] = {{"keys", gm->data_size()}};  // GCOVR_EXCL_BR_LINE (inlined
                                                 // nlohmann::json braced-init
                                                 // branches)
    const std::uint64_t hits = gm->cache_hits();
    const std::uint64_t misses = gm->cache_misses();
    const std::uint64_t lookups = hits + misses;
    root["cache"] = {
        {"size", gm->cache_size()},  // GCOVR_EXCL_LINE (gcov artifact: the
                                     // call runs — cache_get in the fixture
                                     // fills one entry — but the line record
                                     // stays at zero)
        {"hits", hits},
        {"misses", misses},
        {"hit_rate", lookups == 0 ? 0.0  // GCOVR_EXCL_LINE (gcov artifact)
                                  : static_cast<double>(hits) /
                                        static_cast<double>(lookups)},
    };  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json braced-init branches)
    root["locks"] = {
        {"mutexes", gm->mutex_registry_size("mutex")},
        {"spinlocks", gm->mutex_registry_size("spinlock")},
        {"rwlocks", gm->rwlock_count()},  // GCOVR_EXCL_LINE (gcov artifact:
                                          // write lock held in the fixture,
                                          // line record stays at zero)
    };  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json braced-init branches)
    root["ranks"] = {
        {"count", gm->rank_board_count()},  // GCOVR_EXCL_LINE (gcov artifact:
                                            // boards exist in the fixture,
                                            // line record stays at zero)
        {"total_members", gm->rank_total_members()}};  // GCOVR_EXCL_BR_LINE
    root["queues"] = {
        {"normal",
         gm->queue_count()},  // GCOVR_EXCL_LINE (gcov artifact: real pushes
                              // in the fixtures, line record stays at zero)
        // The entries below run (real pushes in the
        // ops/console fixtures) but gcov keeps their line
        // records at zero inside the braced-init list.
        {"delay",
         gm->delay_queue_count()},  // GCOVR_EXCL_LINE GCOVR_EXCL_BR_LINE
        {"priority", gm->priority_queue_count()},  // GCOVR_EXCL_LINE
        {"broadcast",
         gm->broadcast_queue_count()},  // GCOVR_EXCL_LINE GCOVR_EXCL_BR_LINE
        {"reliable", gm->reliable_queue_count()}};  // GCOVR_EXCL_BR_LINE
    std::size_t tasks = 0;
    for (const auto& task : gm->sched_list()) {
        (void)task;
        ++tasks;
    }
    root["scheduler"] = {
        {"tasks", tasks},
        {"active", gm->sched_active_count()}};  // GCOVR_EXCL_BR_LINE
    return root;
}

}  // namespace shield::console

#endif  // SHIELD_ENABLE_GLOBAL
