#include "shield/lua/profile_sampler.hpp"

#include <atomic>
#include <cstddef>

// lua.h itself carries no extern "C" guard; lua.hpp provides the C++
// entry point (include lua.hpp from C++ translation units).
#include <lua.hpp>

namespace shield::lua {

namespace {

// The hook fires while some CAF worker thread executes bytecode of the
// sampled VM. CAF does not pin an actor to one OS thread — successive
// messages of the same service run on whatever worker picks them up — so a
// thread_local slot would be published on one worker and read as null on
// the next. The manager arbitrates at most one sampling session per
// process, hence one global slot; the only writer is the install/uninstall
// fork task, and the hook-side reads observe it atomically. The slot
// carries the sampler's lifetime: sweep_active() runs on every service
// actor thread and a stop path may free the state concurrently, so readers
// take a shared_ptr copy (the sampler owns its session, which keeps the
// aggregation tree alive with it) instead of dereferencing a raw pointer
// that can dangle between the load and the use.
std::atomic<std::shared_ptr<ProfileSampler>> g_active_sampler;

constexpr int kMaxHookDepthFallback = 64;

}  // namespace

ProfileSampler::ProfileSampler(std::shared_ptr<ProfileSession> session,
                               CoProvider co_provider)
    : session_(std::move(session)), co_provider_(std::move(co_provider)) {}

void ProfileSampler::deactivate() {
    // Clear the slot only while it still points at this sampler: the
    // manager arbitrates one session per process, so a second sampler's
    // deactivate must never unpublish the active one. No lua_State is
    // touched — safe on any thread, unlike uninstall(). The load returns a
    // copy, so the object stays alive across the swap even when this runs
    // from the destructor.
    std::shared_ptr<ProfileSampler> current =
        g_active_sampler.load(std::memory_order_acquire);
    if (current.get() != this) {  // GCOVR_EXCL_BR_LINE (defensive: an
        // unpublished sampler's deactivate is unreachable end-to-end —
        // abandon and dtor paths only run for sessions that were handed
        // out; kept so the CAS below never fabricates a shared_ptr of a
        // possibly-freed object)
        return;  // GCOVR_EXCL_LINE (defensive: see above)
    }
    g_active_sampler.compare_exchange_strong(current,
                                             std::shared_ptr<ProfileSampler>());
}

ProfileSampler::~ProfileSampler() {
    // The slot cannot still point here (it held a shared_ptr, so this
    // destructor only runs after the last copy dropped) — the call below
    // is a cheap no-op kept for the invariant it documents.
    deactivate();
}

void ProfileSampler::record_current_stack(ProfileSampler* self, lua_State* L) {
    lua_Debug ar;
    std::vector<ProfileFrame> frames;
    const std::size_t max_depth =
        self ? self->session_->config().max_depth  // GCOVR_EXCL_BR_LINE
             : kMaxHookDepthFallback;  // GCOVR_EXCL_BR_LINE (defensive: hook
                                       // race with uninstall)
    frames.reserve(8);
    for (int level = 0; level < static_cast<int>(max_depth); ++level) {
        if (lua_getstack(L, level, &ar) == 0) {
            break;
        }
        // "n" name (may be absent — main chunk, tail calls), "S" what +
        // source, "l" current line, "t" tail-call flag (5.5 keeps the
        // option; verified by the Task 1 spike).
        if (lua_getinfo(L, "nSlt", &ar) == 0) {  // GCOVR_EXCL_BR_LINE
            // (defensive: the level was just validated by lua_getstack)
            break;  // GCOVR_EXCL_LINE (defensive: see above)
        }
        ProfileFrame f;
        f.what = ar.what ? ar.what : "";  // GCOVR_EXCL_BR_LINE (defensive:
                                          // lua_getinfo always fills what)
        f.name = ar.name ? ar.name : "";
        // short_src truncates at LUA_IDSIZE (60). The spawn loader passes
        // the script path with the '@' file prefix (load_service_module), so
        // the file truncation applies — tail kept, the script file name
        // survives Windows-sized temp paths (C:\Users\... ≈ 62 chars). An
        // unprefixed path would take the *string* truncation instead (head
        // kept, tail cut) and lose the file name entirely — that was the
        // ProfileHappyPathStartStatusStop Windows failure of 2026-09-23.
        // exec_lua uses the '=' literal prefix; dostring-style chunks embed
        // their source text, which short_src renders as [string "..."].
        f.source = ar.short_src[0] != '\0'  // GCOVR_EXCL_BR_LINE (defensive:
                                            // short_src is never empty; the
                                            // first keyword line carries the
                                            // defensive reason)
                       ? ar.short_src
                       : "?";  // GCOVR_EXCL_BR_LINE (compiler artifact:
                               // continuation arms)
        f.line = ar.currentline;
        f.tail = ar.istailcall != 0;
        frames.push_back(std::move(f));
    }
    if (self != nullptr &&  // GCOVR_EXCL_BR_LINE (defensive: the hook
        !frames.empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                            // continuation arms)
        self->session_->add_sample(frames);  // non-empty stack)
    }
}

void ProfileSampler::sampler_hook(lua_State* L, lua_Debug* /*ar*/) {
    // The copy keeps the sampler (and its session) alive for the whole
    // hook body, however the stop path races this firing.
    std::shared_ptr<ProfileSampler> self =
        g_active_sampler.load(std::memory_order_acquire);
    if (self == nullptr) {
        // Stale hook without an active sampler (should not happen: hooks
        // are cleared on uninstall before the sampler dies). Disarm.
        lua_sethook(L, nullptr, 0, 0);
        return;
    }
    record_current_stack(self.get(), L);

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
        return;           // GCOVR_EXCL_LINE (defensive: see above)
    }
    for (lua_State* co : co_provider_()) {
        if (co != nullptr &&      // GCOVR_EXCL_BR_LINE (race: coroutine already
                                  // carries this hook)
            lua_gethook(co) !=    // GCOVR_EXCL_BR_LINE (compiler
                                  // artifact: inline call throw arc)
                &sampler_hook) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // inline arcs + race)
            // GCOVR_EXCL_START (compiler artifact: inline accessor arcs on
            // config())
            lua_sethook(co, &sampler_hook, LUA_MASKCOUNT,
                        static_cast<int>(session_->config().interval));
            // GCOVR_EXCL_STOP
        }
    }
}

void ProfileSampler::sweep_active(std::string_view service_id) {
    // Single shared_ptr load: the copy keeps the sampler alive even if a
    // concurrent stop path clears the slot and drops its own reference
    // mid-call (the old raw load-then-deref was a use-after-free window).
    // The service binding is the thread-safety gate: sweep_active runs on
    // arbitrary service actor threads, while sweep_once walks that
    // sampler's registered lua_State values and may call lua_sethook. Only
    // the owner service's dispatch thread may touch its sampler's
    // coroutines; every other service drive point is a no-op.
    std::shared_ptr<ProfileSampler> sampler =
        g_active_sampler.load(std::memory_order_acquire);
    if (sampler != nullptr && sampler->service_id_ == service_id) {
        sampler->sweep_once();
    }
}

void ProfileSampler::install(lua_State* main_L, std::string service_id) {
    // Save, then arm, then publish — a hook firing between sethook and the
    // publish would find no sampler and disarm itself. The publish stores
    // shared_from_this(): the caller handed this sampler to the session
    // state (a shared_ptr) before install ran, so the ownership is live.
    saved_hook_ = lua_gethook(main_L);
    saved_mask_ = lua_gethookmask(main_L);
    saved_count_ = lua_gethookcount(main_L);
    main_L_ = main_L;
    service_id_ = std::move(service_id);

    g_active_sampler.store(shared_from_this(), std::memory_order_release);
    lua_sethook(main_L_, &sampler_hook, LUA_MASKCOUNT,
                static_cast<int>(session_->config().interval));

    // Arm already-live coroutines (b3 spike: arming a suspended coroutine
    // works and takes effect on its next resume).
    if (co_provider_) {
        for (lua_State* co : co_provider_()) {
            if (co != nullptr) {
                lua_sethook(co, &sampler_hook, LUA_MASKCOUNT,
                            static_cast<int>(session_->config().interval));
            }
        }
    }
}

void ProfileSampler::uninstall(lua_State* main_L) {
    // Unpublish first so a hook firing mid-teardown finds nothing and
    // disarms itself instead of recording into a dying session.
    g_active_sampler.store(nullptr, std::memory_order_release);
    if (main_L != nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: callers pass
        // the main state of the still-alive service VM)

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
