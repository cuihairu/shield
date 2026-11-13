// [SHIELD_LUA] /ops/profile sampling hook — install/accumulate/uninstall
//
// A count-hook sampler for one service's Lua VM. Everything (install,
// uninstall, the hook itself, rescan sweeps) runs on the owning service's
// actor thread: a forked task serializes against every resume source, so
// no lua_State is touched from another thread and the sampler keeps no
// locks. See docs/superpowers/plans/2026-09-22-ops-profile-v1.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "shield/lua/profile_session.hpp"

struct lua_State;
struct lua_Debug;

namespace shield::lua {

/// @brief Owner-thread sampling session bound to one VM.
///
/// The active sampler is found by the C hook through a process-global
/// thread-safe slot: the manager arbitrates at most one session per
/// process, and CAF does not pin an actor to one OS thread, so a
/// thread_local slot would be published by one worker and read as null by
/// the next.
///
/// The slot holds a shared_ptr, not a raw pointer: sweep_active() runs at
/// the coroutine drive points of every service actor thread, and a
/// concurrent stop path may destroy the session state while such a reader
/// holds the loaded pointer (the old load-then-deref was a use-after-free
/// window). Readers load the shared_ptr, so the sampler — and the
/// ProfileSession it owns — stay alive until the last in-flight reader is
/// done; the slot is cleared promptly by deactivate()/uninstall() and the
/// object frees itself when the last copy drops.
class ProfileSampler : public std::enable_shared_from_this<ProfileSampler> {
public:
    /// Owner-thread collection of this service's live coroutines (plain
    /// pointers, safe to use only on the owner thread while the session
    /// is installed). The provider collects its list under the registry
    /// lock and resolves nothing on other threads.
    using CoProvider = std::function<std::vector<lua_State*>()>;

    /// Every kRescanEvery hook hits the sampler re-arms hooks on live
    /// coroutines: Lua 5.5 does NOT propagate a new hook to coroutines
    /// created later (Task 1 spike), so new handler coroutines are picked
    /// up by the next sweep.
    static constexpr int kRescanEvery = 100;

    /// The sampler owns its session: an in-flight reader holding a slot
    /// copy keeps both alive past the manager-side state teardown.
    ProfileSampler(std::shared_ptr<ProfileSession> session,
                   CoProvider co_provider);
    ~ProfileSampler();

    ProfileSampler(const ProfileSampler&) = delete;
    ProfileSampler& operator=(const ProfileSampler&) = delete;

    /// @brief Arm the count hook on the given main state and every live
    /// coroutine, and publish this sampler on the thread_local slot.
    /// `main_L` must be resolved on the owner thread (vm_main_state).
    /// @param service_id Owner service; sweep_active() only sweeps when the
    ///        calling thread is executing this service's dispatch context.
    void install(lua_State* main_L, std::string service_id);

    /// @brief Restore the saved main-state hook, clear hooks from live
    /// coroutines, and unpublish the thread_local slot.
    void uninstall(lua_State* main_L);

    /// @brief Unpublish this sampler from the global slot without touching
    /// any lua_State, and revoke it: a lagging install task that runs after
    /// this call detects the revoke and no-ops instead of republishing
    /// (abandon can win the race against the install it was handed out to).
    ///
    /// Uninstall must run on the owning actor thread (it calls lua_sethook);
    /// abandon paths run on foreign teardown threads, so they clear the
    /// slot with this instead. After it returns a firing hook observes the
    /// null slot and disarms itself, and sweep_active() becomes a no-op.
    /// The object itself stays alive until the last shared_ptr copy drops
    /// (the slot and every in-flight hook hold one).
    void deactivate();

    /// @brief Hook entry (LUA_MASKCOUNT). Public so it can be named by
    /// lua_sethook; never call it directly.
    static void sampler_hook(lua_State* L, lua_Debug* ar);

    /// @brief Re-arm hooks on this thread's live coroutines, when a
    /// sampler is active on this thread. Called at the coroutine drive
    /// points (invoke_coroutine / resume_suspended_caller): Lua 5.5 does
    /// not propagate hooks to coroutines created after install, handler
    /// coroutines are brand-new per message, and the main state itself
    /// never executes bytecode in a coroutine-driven service — so the
    /// in-hook sweep would never fire. One thread_local read when no
    /// session is active.
    /// @brief Re-arm hooks on this thread's live coroutines, when a sampler
    ///        bound to @p service_id is active on this thread. Called at the
    ///        coroutine drive points: a sampler for a different service must
    ///        never touch another service's lua_State from this thread.
    static void sweep_active(std::string_view service_id);

private:
    static void record_current_stack(ProfileSampler* self, lua_State* L);
    void sweep_once();

    lua_State* main_L_;
    std::shared_ptr<ProfileSession> session_;
    CoProvider co_provider_;
    std::string service_id_;

    // Saved main-state hook state (restored on uninstall; in practice no
    // other hook user exists — the sampler is the sole owner — but the
    // save/restore discipline is kept regardless).
    void (*saved_hook_)(lua_State*, lua_Debug*) = nullptr;
    int saved_mask_ = 0;
    int saved_count_ = 0;

    int hits_ = 0;  ///< hook invocations since install (drives rescan)

    // Set by deactivate(): a revoked sampler is un-publishable — install()
    // checks this before arming and again after publishing (the abandon
    // path can run between the manager's handout and the install task's
    // execution), so an abandoned sampler can never reappear on the slot.
    std::atomic<bool> revoked_{false};
};

}  // namespace shield::lua
