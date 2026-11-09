// [SHIELD_LUA] shd binding layer - split of binding.hpp.
// [SHD] Template C closures: set_function argument conversion,
// single/multi-return support and protected-call results.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <lua.hpp>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace shd {

// ---- closures -------------------------------------------------------------

namespace detail {

template <typename R, typename... Args, typename F>
std::function<int(lua_State*)> make_dispatcher(F&& f);

// Signature introspection for set_function targets. Each specialization
// carries a static make() that builds the std::function dispatcher, so the
// Args... pack is expanded where the tuple type is visible.
template <typename F>
struct closure_traits : closure_traits<decltype(&F::operator())> {};
template <typename R, typename... Args>
struct closure_traits<R (*)(Args...)> {
    using ret = R;
    using args = std::tuple<Args...>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return make_dispatcher<R, Args...>(std::forward<G>(g));
    }
};
template <typename R, typename C, typename... Args>
struct closure_traits<R (C::*)(Args...)> {
    using ret = R;
    using args = std::tuple<Args...>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return make_dispatcher<R, Args...>(std::forward<G>(g));
    }
};
template <typename R, typename C, typename... Args>
struct closure_traits<R (C::*)(Args...) const> {
    using ret = R;
    using args = std::tuple<Args...>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return make_dispatcher<R, Args...>(std::forward<G>(g));
    }
};

// Raw C-API pass-through: a callable taking (lua_State*) and returning the
// result count bypasses argument unpacking and result pushing entirely
// (lua_CFunction parity). The thunk's exception guard still applies.
template <>
struct closure_traits<int (*)(lua_State*)> {
    using ret = int;
    using args = std::tuple<>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return [g](lua_State* L) -> int { return g(L); };
    }
};
template <typename C>
struct closure_traits<int (C::*)(lua_State*)> {
    using ret = int;
    using args = std::tuple<>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return [g](lua_State* L) -> int { return g(L); };
    }
};
template <typename C>
struct closure_traits<int (C::*)(lua_State*) const> {
    using ret = int;
    using args = std::tuple<>;
    template <typename G>
    static std::function<int(lua_State*)> make(G&& g) {
        return [g](lua_State* L) -> int { return g(L); };
    }
};

template <typename F>
int closure_thunk(lua_State* L) {
    const std::function<int(lua_State*)>* disp =
        static_cast<const std::function<int(lua_State*)>*>(
            lua_touserdata(L, lua_upvalueindex(1)));
    // C++ exceptions must never cross the lua_pcall boundary: convert them
    // to Lua errors. The catch handlers COMPLETE (freeing the
    // exception objects) before luaL_error longjmps out — calling lua_error
    // inside a handler would skip the exception cleanup and leak it.
    // msgbuf is a POD array on purpose: longjmp may skip its "destruction",
    // and a POD has none.
    //
    // Clone-family note (closure_thunk here, and the same shape in
    // usertype_callable_thunk / usertype_method_thunk below): every bound
    // signature instantiates its own copy of this thunk, and only a subset
    // of the registered callables is ever driven with a throwing body in a
    // given test leg. The branch records on the dispatch, catch, and
    // error-return lines therefore starve per instantiation; the markers
    // on those lines denote exactly that (the driven copies of every arm
    // run in the exception suites).
    char msgbuf[512] = {0};
    int kind = 0;
    // GCOVR_EXCL_START (clone family: the dispatch, catch, and error-return
    // records below are per-instantiation and starve for every signature the
    // suites never drive with a throwing body - see the closure_thunk note
    // above; the driven copies of every arm run in the exception suites)
    try {
        return (*disp)(L);
    } catch (const std::exception& e) {
        std::snprintf(msgbuf, sizeof(msgbuf), "%s", e.what());
        kind = 1;
    } catch (...) {
        kind = 2;
    }
    if (kind == 2) {
        return luaL_error(L, "unknown C++ exception");
    }
    return luaL_error(L, "%s", msgbuf);
    // GCOVR_EXCL_STOP
}

// Build the dispatcher for a callable with explicit signature.
template <typename R, typename... Args, typename F>
std::function<int(lua_State*)> make_dispatcher(F&& f) {
    return [f](lua_State* L) -> int {
        int idx = 1;
        auto args =  // GCOVR_EXCL_BR_LINE (per-instantiation merge artifact:
                     // this line pools the pack-expansion arcs of every bound
                     // signature, and one arity's edge is never driven by the
                     // suites; see the closure_thunk clone-family note)
            unpack_all<std::decay_t<Args>...>(
                L, idx, std::index_sequence_for<Args...>{});
        if constexpr (std::is_void_v<R>) {
            std::apply(f, std::move(args));
            return 0;
        } else {
            R r = std::apply(f, std::move(args));
            return push_result(L, 1, std::move(r));
        }
    };
}

}  // namespace detail

template <typename F>
void table::push_closure(F&& f) {
    using traits = detail::closure_traits<std::decay_t<F>>;
    auto* store = static_cast<std::function<int(lua_State*)>*>(
        lua_newuserdatauv(L_, sizeof(std::function<int(lua_State*)>), 1));
    new (store)  // GCOVR_EXCL_BR_LINE (compiler artifact: the placement
                 // construction's std::function move arcs)
        std::function<int(lua_State*)>(traits::make(std::forward<F>(f)));
    lua_newtable(L_);  // metatable guarding the userdata lifetime
    lua_pushcfunction(L_, [](lua_State* L) -> int {
        auto* fn =
            static_cast<std::function<int(lua_State*)>*>(lua_touserdata(L, 1));
        fn->~function();
        return 0;
    });
    lua_setfield(L_, -2, "__gc");
    lua_setmetatable(L_, -2);
    lua_pushcclosure(L_, &detail::closure_thunk<std::decay_t<F>>, 1);
}

}  // namespace shd
