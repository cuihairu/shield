#include "shield/lua/profile_sampler.hpp"

#include <cstddef>

// This tree's lua.h carries no extern "C" guard — the C++ entry point is
// lua.hpp (same include discipline as sol2).
#include <lua.hpp>

namespace shield::lua {

namespace {

// The hook fires while the owner thread is executing bytecode, so the
// thread_local lookup can never cross threads. At most one sampling
// session exists per process (manager-side arbitration), hence at most
// one sampler per thread.
thread_local ProfileSampler* t_active_sampler = nullptr;

constexpr int kMaxHookDepthFallback = 64;

}  // namespace

ProfileSampler::ProfileSampler(ProfileSession& session, CoProvider co_provider)
    : session_(session), co_provider_(std::move(co_provider)) {}

ProfileSampler::~ProfileSampler() {
    // Defensive: an uninstall that never ran must not leave the TLS slot
    // dangling. Hooks themselves die with the VM.
    if (t_active_sampler == this) {
        t_active_sampler = nullptr;
    }
}

void ProfileSampler::record_current_stack(lua_State* L) {
    lua_Debug ar;
    std::vector<ProfileFrame> frames;
    const std::size_t max_depth =
        t_active_sampler ? t_active_sampler->session_.config().max_depth
                         : kMaxHookDepthFallback;
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
    if (t_active_sampler != nullptr && !frames.empty()) {
        t_active_sampler->session_.add_sample(frames);
    }
}

void ProfileSampler::sampler_hook(lua_State* L, lua_Debug* /*ar*/) {
    ProfileSampler* self = t_active_sampler;
    if (self == nullptr) {
        // Stale hook without an active sampler (should not happen: hooks
        // are cleared on uninstall before the sampler dies). Disarm.
        lua_sethook(L, nullptr, 0, 0);
        return;
    }
    record_current_stack(L);

    // Periodic re-arm sweep: coroutines created since install (Lua 5.5
    // does not propagate hooks to new threads — Task 1 spike) pick the
    // hook up here. The provider collects under the registry lock and the
    // sweep runs back on the owner thread, mirroring inspect_coroutines'
    // safety argument (the owner thread serializes against every resume
    // source, so no coroutine is being driven right now).
    ++self->hits_;
    if (self->hits_ % kRescanEvery == 0 && self->co_provider_) {
        for (lua_State* co : self->co_provider_()) {
            if (co != nullptr && lua_gethook(co) != &sampler_hook) {
                lua_sethook(co, &sampler_hook, LUA_MASKCOUNT,
                            static_cast<int>(self->session_.config().interval));
            }
        }
    }
}

bool ProfileSampler::active_on_this_thread() {
    return t_active_sampler != nullptr;
}

void ProfileSampler::install(lua_State* main_L) {
    // Save, then arm, then publish — a hook firing between sethook and
    // the TLS publish would find no sampler and disarm itself.
    saved_hook_ = lua_gethook(main_L);
    saved_mask_ = lua_gethookmask(main_L);
    saved_count_ = lua_gethookcount(main_L);
    main_L_ = main_L;

    t_active_sampler = this;
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
    t_active_sampler = nullptr;
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
