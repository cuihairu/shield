#include "shield/lua/profile_sampler.hpp"

#include <atomic>
#include <cstddef>

// This tree's lua.h carries no extern "C" guard — the C++ entry point is
// lua.hpp (same include discipline as sol2).
#include <lua.hpp>

namespace shield::lua {

namespace {

// The hook fires while some CAF worker thread executes bytecode of the
// sampled VM. CAF does not pin an actor to one OS thread — successive
// messages of the same service run on whatever worker picks them up — so a
// thread_local slot would be published on one worker and read as null on
// the next. The manager arbitrates at most one sampling session per
// process, hence one global slot; the only writer is the install/uninstall
// fork task, and the hook-side reads observe it atomically.
std::atomic<ProfileSampler*> g_active_sampler{nullptr};

constexpr int kMaxHookDepthFallback = 64;

}  // namespace

ProfileSampler::ProfileSampler(ProfileSession& session, CoProvider co_provider)
    : session_(session), co_provider_(std::move(co_provider)) {}

ProfileSampler::~ProfileSampler() {
    // Defensive: an uninstall that never ran must not leave the slot
    // dangling. Hooks themselves die with the VM.
    ProfileSampler* expected = this;
    g_active_sampler.compare_exchange_strong(expected, nullptr);
}

void ProfileSampler::record_current_stack(ProfileSampler* self, lua_State* L) {
    lua_Debug ar;
    std::vector<ProfileFrame> frames;
    const std::size_t max_depth =
        self ? self->session_.config().max_depth : kMaxHookDepthFallback;
    frames.reserve(8);
    for (int level = 0; level < static_cast<int>(max_depth); ++level) {
        if (lua_getstack(L, level, &ar) == 0) {
            break;
        }
        // "n" name (may be absent — main chunk, tail calls), "S" what +
        // source, "l" current line, "t" tail-call flag (5.5 keeps the
        // option; verified by the Task 1 spike).
        if (lua_getinfo(L, "nSlt", &ar) == 0) {
            break;
        }
        ProfileFrame f;
        f.what = ar.what ? ar.what : "";
        f.name = ar.name ? ar.name : "";
        f.source = ar.short_src[0] != '\0' ? ar.short_src : "?";
        f.line = ar.currentline;
        f.tail = ar.istailcall != 0;
        frames.push_back(std::move(f));
    }
    if (self != nullptr && !frames.empty()) {
        self->session_.add_sample(frames);
    }
}

void ProfileSampler::sampler_hook(lua_State* L, lua_Debug* /*ar*/) {
    ProfileSampler* self = g_active_sampler.load(std::memory_order_acquire);
    if (self == nullptr) {
        // Stale hook without an active sampler (should not happen: hooks
        // are cleared on uninstall before the sampler dies). Disarm.
        lua_sethook(L, nullptr, 0, 0);
        return;
    }
    record_current_stack(self, L);

    // Periodic re-arm sweep for main-state bytecode execution (exec_lua and
    // friends). Coroutine-driven services never run main-state bytecode —
    // their new handler coroutines are armed at the drive points instead
    // (sweep_once at invoke_coroutine / resume_suspended_caller).
    ++self->hits_;
    if (self->hits_ % kRescanEvery == 0) {
        self->sweep_once();
    }
}

void ProfileSampler::sweep_once() {
    // The provider collects under the registry lock and the sweep runs on
    // the driving thread, mirroring inspect_coroutines' safety argument
    // (the driving thread holds the actor's execution, so no coroutine of
    // this service is running elsewhere right now).
    if (!co_provider_) {  // GCOVR_EXCL_BR_LINE (defensive: both production
                          // (service provider) and test providers are always
                          // set; a default-constructed provider is unreachable)
        return;
    }
    for (lua_State* co : co_provider_()) {
        if (co != nullptr && lua_gethook(co) != &sampler_hook) {
            lua_sethook(co, &sampler_hook, LUA_MASKCOUNT,
                        static_cast<int>(session_.config().interval));
        }
    }
}

void ProfileSampler::sweep_active() {
    if (g_active_sampler.load(std::memory_order_acquire) != nullptr) {
        g_active_sampler.load(std::memory_order_relaxed)->sweep_once();
    }
}

bool ProfileSampler::active_on_this_thread() {
    return g_active_sampler.load(std::memory_order_acquire) != nullptr;
}

void ProfileSampler::install(lua_State* main_L) {
    // Save, then arm, then publish — a hook firing between sethook and the
    // publish would find no sampler and disarm itself.
    saved_hook_ = lua_gethook(main_L);
    saved_mask_ = lua_gethookmask(main_L);
    saved_count_ = lua_gethookcount(main_L);
    main_L_ = main_L;

    g_active_sampler.store(this, std::memory_order_release);
    lua_sethook(main_L_, &sampler_hook, LUA_MASKCOUNT,
                static_cast<int>(session_.config().interval));

    // Arm already-live coroutines (b3 spike: arming a suspended coroutine
    // works and takes effect on its next resume).
    if (co_provider_) {
        for (lua_State* co : co_provider_()) {
            if (co != nullptr) {
                lua_sethook(co, &sampler_hook, LUA_MASKCOUNT,
                            static_cast<int>(session_.config().interval));
            }
        }
    }
}

void ProfileSampler::uninstall(lua_State* main_L) {
    // Unpublish first so a hook firing mid-teardown finds nothing and
    // disarms itself instead of recording into a dying session.
    g_active_sampler.store(nullptr, std::memory_order_release);
    if (main_L != nullptr) {
        lua_sethook(main_L, saved_hook_, saved_mask_, saved_count_);
    }
    if (co_provider_) {
        for (lua_State* co : co_provider_()) {
            if (co != nullptr) {
                // No other hook user exists (Task 1 spike: the sampler is
                // the sole owner), so clearing is the correct restore.
                lua_sethook(co, nullptr, 0, 0);
            }
        }
    }
    hits_ = 0;
}

}  // namespace shield::lua
