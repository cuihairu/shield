// [SHIELD_LUA] shd binding layer - split of binding.hpp.
// [SHD] Vocabulary and stack primitives: usertype marker traits,
// type/nil/this_state, registry-ref adoption, the usertype meta
// vocabulary, and the stack push/traits + argument unpack layer.
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

// ---- usertype marker ------------------------------------------------------

// Opt-in marker for host types that carry a registered shd usertype. A value
// of such a type pushes as userdata with its registered metatable
// (parity for `tbl["k"] = handle` and make_object(state, box)). Specialize
// this in the same header that defines the type; unrelated class types
// (nlohmann::json, view wrappers) deliberately stay unmarked so a missing
// push path fails at compile time instead of pushing a zeroed userdata.
template <typename T>
struct is_usertype_value : std::false_type {};

// Marker for usertypes registered by a foreign binding (not shd).
// These types use pointer-box layout (heap-allocated payload) and share the
// foreign metatable. shd::push will allocate on the heap and use the
// foreign metatable so __gc works correctly.
template <typename T>
struct is_foreign_usertype : std::false_type {};

// ---- type -----------------------------------------------------------------

enum class type {
    none = LUA_TNONE,
    nil = LUA_TNIL,
    boolean = LUA_TBOOLEAN,
    lightuserdata = LUA_TLIGHTUSERDATA,
    number = LUA_TNUMBER,
    string = LUA_TSTRING,
    table = LUA_TTABLE,
    function = LUA_TFUNCTION,
    userdata = LUA_TUSERDATA,
    thread = LUA_TTHREAD,
};

inline const char* type_name(lua_State* L, int idx) {
    return luaL_typename(L, idx);
}

// ---- nil ------------------------------------------------------------------

struct nil_t {};
inline constexpr nil_t nil{};
inline constexpr std::nullopt_t nullopt = std::nullopt;

// optional parity: common Lua binding layers model optional values as
// std::optional with some conveniences. We expose std::optional directly under
// the shd namespace.
template <typename T>
using optional = std::optional<T>;

// ---- this_state -----------------------------------------------------------

// Lightweight carrier handed to set_function lambdas that need the raw
// lua_State* (compatible shape: constructible from lua_State*).
struct this_state {
    lua_State* L;
    this_state(lua_State* l) : L(l) {}
    operator lua_State*() const { return L; }
};

// ---- registry ref adoption -------------------------------------------------

// Tag for constructing a reference from an existing LUA_REGISTRYINDEX entry.
// The constructor pushes and re-refs the value, so both the source handle and
// the new object own independent registry entries (no double-unref).
struct ref_index_tag {
    int ref;
};
inline ref_index_tag ref_index(int r) { return ref_index_tag{r}; }

// ---- usertype vocabulary ----------------------------------------------------

namespace meta_function {
struct to_string_t {};
inline constexpr to_string_t to_string{};
struct equal_to_t {};
inline constexpr equal_to_t equal_to{};
}  // namespace meta_function

struct no_constructor_t {};
inline constexpr no_constructor_t no_constructor{};

// Property parity: wraps a lambda evaluated on read (value
// semantics), bound by new_usertype into the usertype's property table.
template <typename F>
struct property_fn {
    F f;
};
template <typename F>
property_fn<F> property(F&& f) {
    return property_fn<F>{std::forward<F>(f)};
}

// ---- stack_object -----------------------------------------------------------

// Non-owning view of a value at a fixed stack position.
// The index is absolutized on construction; the
// view is only valid while that stack slot still holds the value. Defined
// in full after the detail conversion traits.
class object;
class stack_object;

// lua_State* of the main thread of the VM that owns L (registry
// LUA_RIDX_MAINTHREAD). Coroutines share the registry, so functions are
// re-anchored to the main thread before being stored beyond the coroutine's
// lifetime.
inline lua_State* main_thread(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main != nullptr ? main :  // GCOVR_EXCL_BR_LINE (defensive:
                                     // LUA_RIDX_MAINTHREAD always holds the
                                     // live main thread on luaL_newstate VMs,
                                     // so the null arm never fires)
               L;
}

// ---- stack push/traits ----------------------------------------------------

// Referencing classes defined further down; declared here at shd scope so
// the detail:: declarations below (and dependent lookups in templates) bind
// to the real types.
class table;
class table_iterator;
class object;
class function;
class accessor;
class variadic_results;
class variadic_args;
class stack_object;
class protected_function;
class protected_function_result;
class state_view;
class state;

template <typename T, typename... Args>
void new_usertype(state_view sv, const std::string& name, Args&&... args);

// Range wrapper expanding a container as successive call arguments (as_args
// parity); defined after variadic_results.
template <typename Container>
struct as_args_t;

namespace detail {

// Push a value of any supported host type onto the stack.
inline void push(lua_State* L, nil_t) { lua_pushnil(L); }
inline void push(lua_State* L, bool v) { lua_pushboolean(L, v ? 1 : 0); }
inline void push(lua_State* L, std::int64_t v) { lua_pushinteger(L, v); }
inline void push(lua_State* L, int v) { lua_pushinteger(L, v); }
inline void push(lua_State* L, std::uint64_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
}
inline void push(lua_State* L, std::uint32_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
}
inline void push(lua_State* L, double v) { lua_pushnumber(L, v); }
inline void push(lua_State* L, float v) { lua_pushnumber(L, v); }
inline void push(lua_State* L, const char* v) {
    lua_pushlstring(L, v, std::strlen(v));
}
inline void push(lua_State* L, std::string_view v) {
    lua_pushlstring(L, v.data(), v.size());
}
inline void push(lua_State* L, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
}

// Arithmetic spellings with no exact overload above (`long long` — which is
// lua_Integer itself — plus short/unsigned/long double). The non-template
// overloads are exact matches for the common types and win overload ties, so
// this only ever catches what they miss.
template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T> &&
                                                  !std::is_same_v<T, bool>>>
inline void push(lua_State* L, T v) {
    if constexpr (std::is_floating_point_v<T>)
        lua_pushnumber(L, static_cast<lua_Number>(v));
    else
        lua_pushinteger(L, static_cast<lua_Integer>(v));
}

void push(lua_State* L, const table& t);
void push(lua_State* L, const object& o);
void push(lua_State* L, const function& f);

// Registered-usertype class branch of is<>/as<> (defined after the type-name
// registry; stack_object is defined before object, so the class branch goes
// through these (L, idx) helpers instead of constructing an object).
template <typename D>
bool usertype_is(lua_State* L, int idx);
template <typename D>
D& usertype_as(lua_State* L, int idx);

// Install a __gc metamethod for a specific type T that handles both
// shd-created (raw T in-place, tagged with uservalue[1]=type name) and
// foreign-created (boxed) userdata. The original __gc is preserved
// under the key "__shd_prev_gc" in the metatable.
template <typename T>
inline void install_dual_layout_gc(lua_State* L, const char* type_name) {
    luaL_getmetatable(L, type_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    // Save original __gc under "__shd_prev_gc"
    lua_getfield(L, -1, "__gc");
    if (!lua_isnil(L, -1)) {
        lua_setfield(L, -2, "__shd_prev_gc");
    } else {
        lua_pop(L, 1);
    }
    // Install new __gc that dispatches based on uservalue 1
    lua_pushcfunction(L, [](lua_State* L) -> int {
        // Userdata is at index 1
        if (lua_type(L, 1) != LUA_TUSERDATA) return 0;
        // Check uservalue 1 (shd layout tag: the registered type name)
        lua_getiuservalue(L, 1, 1);
        bool is_shd_layout = (lua_type(L, -1) == LUA_TSTRING);
        lua_pop(L, 1);  // pop uservalue
        if (is_shd_layout) {
            // shd layout: raw T in-place at userdata pointer
            auto* p = static_cast<T*>(lua_touserdata(L, 1));
            if (p) p->~T();
            return 0;
        }
        // foreign-box layout: call the preserved original __gc
        lua_getmetatable(L, 1);
        lua_getfield(L, -1, "__shd_prev_gc");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);  // userdata as argument
            lua_call(L, 1, 0);
        }
        lua_pop(L, 1);  // pop metatable
        return 0;
    });
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);  // pop metatable
}

template <typename T>
void push(lua_State* L, const std::optional<T>& v) {
    if (v)  // GCOVR_EXCL_BR_LINE (instantiation artifact: the taken-arm of
            // push<optional<int64_t>> is driven only by the deadline shim's
            // non-null return, which no coverage suite reaches - see its
            // GCOVR_EXCL_LINE)
        push(L, *v);
    else
        push(L, nil);
}

// The primitive read/push layer is keyed on *categories*, not on individual
// type spellings. `lua_Integer` is `long long` while `std::int64_t` is `long`
// on LP64, so code that says `as<lua_Integer>()` must compile here — writing
// one full specialization per spelling would leave `long long` (and `short`,
// `unsigned int`, ...) unbound and fail at link time. Categories keep the
// strict semantics (integers only via lua_isinteger, strings only via
// LUA_TSTRING) while covering every arithmetic type.

// Type-tagged stack check: can the value at idx be read as T?
template <typename T>
struct is_stack : std::bool_constant<std::is_arithmetic_v<T> ||
                                     std::is_same_v<T, std::string> ||
                                     std::is_same_v<T, const char*>> {};

template <typename T>
inline bool stack_check(lua_State* L, int idx) {
    if constexpr (std::is_same_v<T, bool>) {
        return lua_isboolean(L, idx) != 0;
    } else if constexpr (std::is_integral_v<T>) {
        // Integer binds accept only true Lua integers (a float
        // argument is a type error, not a silent truncation).
        return lua_isinteger(L, idx) != 0;
    } else if constexpr (std::is_floating_point_v<T>) {
        return lua_isnumber(L, idx) != 0;
    } else {
        // String binds accept only actual strings (numbers do not coerce).
        return lua_type(L, idx) == LUA_TSTRING;
    }
}

// Read a value of type T off the stack.
template <typename T>
inline T stack_read(lua_State* L, int idx) {
    if constexpr (std::is_same_v<T, bool>) {
        return lua_toboolean(L, idx) != 0;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(lua_tointeger(L, idx));
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(lua_tonumber(L, idx));
    } else if constexpr (std::is_same_v<T, std::string>) {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        return std::string(
            s ? s : "",    // GCOVR_EXCL_BR_LINE (defensive:
                           // stack_check gates the read to
                           // LUA_TSTRING, so tolstring never
                           // returns null here)
            s ? len : 0);  // GCOVR_EXCL_BR_LINE (defensive: same guard -
                           // tolstring writes len whenever it yields a string)
    } else if constexpr (std::is_same_v<T, const char*>) {
        return lua_tostring(L, idx);
    } else {
        static_assert(!sizeof(T*), "shd: type is not readable off the stack");
        return T{};
    }
}

// Lua type name for a host type (for error messages).
template <typename T>
const char* host_type_name() {
    if constexpr (std::is_same_v<T, bool>) {
        return "boolean";
    } else if constexpr (std::is_integral_v<T>) {
        return "integer";
    } else if constexpr (std::is_floating_point_v<T>) {
        return "number";
    } else if constexpr (std::is_same_v<T, std::string> ||
                         std::is_same_v<T, const char*>) {
        return "string";
    } else {
        return "value";
    }
}

// Argument unpacking for closures: recursive index walk.
template <typename... Args>
struct args_count;
template <>
struct args_count<> : std::integral_constant<int, 0> {};
template <typename Head, typename... Rest>
struct args_count<Head, Rest...>
    : std::integral_constant<int, 1 + args_count<Rest...>::value> {};

template <typename T>
struct is_optional : std::false_type {};
template <typename V>
struct is_optional<std::optional<V>> : std::true_type {};

template <typename T>
T unpack_arg(lua_State* L, int& idx) {
    // Reference/const-qualified argument shapes normalize to D here; the
    // returned D converts back to T at the return statement.
    using D = std::remove_const_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<D, this_state>) {
        (void)idx;
        return this_state(L);
    } else if constexpr (std::is_same_v<D, lua_State*>) {
        (void)idx;
        return L;
    } else if constexpr (std::is_same_v<D, variadic_args>) {
        const int from = idx;
        const int to = lua_gettop(L) + 1;
        idx = to;
        // Construct via the dependent D: the target class (defined later in
        // this header) is incomplete here, and clang checks non-dependent
        // constructions eagerly at the definition.
        return D(L, from, to);
    } else if constexpr (std::is_same_v<D, object>) {
        const int i = idx++;
        return D(L, i);
    } else if constexpr (std::is_same_v<D, table>) {
        const int i = idx++;
        return D(L, i);
    } else if constexpr (std::is_same_v<D, function>) {
        const int i = idx++;
        return D(L, i);
    } else if constexpr (std::is_same_v<D, protected_function>) {
        const int i = idx++;
        return D(L, i);
    } else if constexpr (is_optional<D>::value) {
        const int i = idx++;
        // An optional parameter accepts absent arguments too
        // (slots past the top are LUA_TNONE, not nil).
        if (lua_isnoneornil(L, i)) return D();
        using V = typename D::value_type;
        int j = i;
        return D(unpack_arg<V>(L, j));
    } else {
        const int i = idx++;
        if (!stack_check<D>(L, i)) {
            luaL_error(L, "bad argument #%d (%s expected, got %s)", i,
                       host_type_name<D>(), luaL_typename(L, i));
            // luaL_error longjmps into lua_pcall; the throw below is
            // unreachable and only satisfies the return type.
            throw std::runtime_error(  // GCOVR_EXCL_LINE (unreachable:
                                       // luaL_error longjmps past it)
                "shd::lua: argument conversion failed");
        }
        D v = stack_read<D>(L, i);
        return v;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH landing
                   // arcs this return sequence records never run)
    }
}

template <typename... Args, std::size_t... I>
std::tuple<Args...> unpack_all(lua_State* L, int& idx,
                               std::index_sequence<I...>) {
    // Braces, not parentheses: pack expansion into a braced-init-list is
    // sequenced left-to-right (guaranteed), so the arguments map onto the
    // Lua stack positions in order. A parenthesized ctor call would leave
    // the evaluation order unspecified (GCC unwinds right-to-left and
    // silently reverses the arguments).
    return std::tuple<Args...>{unpack_arg<Args>(  // GCOVR_EXCL_BR_LINE
        L, idx)...};  // GCOVR_EXCL_BR_LINE (compiler artifact: pack-expansion
                      // init arcs record per instantiation)
}

// Usertype push (defined with the type-name registry below): forward
// declared here so the dependent push() call inside push_result sees it.
// Ordinary lookup for that dependent call only considers declarations above
// it; box types from other namespaces (e.g. shield::lua) do not trigger
// ADL into shd::detail, so without this the by-value usertype return path
// in self_dispatch_helper has no viable push overload.
template <typename T,
          typename Unused = std::enable_if_t<is_usertype_value<T>::value>>
void push(lua_State* L, const T& v);

// Result pushing: single value, void, or tuple for multi-return.
inline int push_result(lua_State* L, int base) {
    (void)base;
    return 0;
}
template <typename T>
int push_result(lua_State* L, int base, T&& v) {
    (void)base;
    push(L, std::forward<T>(v));
    return 1;
}
template <typename... Ts, std::size_t... I>
int push_result(lua_State* L, int base, std::tuple<Ts...>&& t,
                std::index_sequence<I...>) {
    (void)base;
    (push(L, std::get<I>(std::move(t))), ...);
    return static_cast<int>(sizeof...(Ts));
}
template <typename... Ts>
int push_result(lua_State* L, int base, std::tuple<Ts...>&& t) {
    return push_result(L, base, std::move(t), std::index_sequence_for<Ts...>{});
}

// Defined after class variadic_results; declared here so the dependent
// push_result calls below resolve to it at instantiation time.
int push_result(lua_State* L, int base, variadic_results&& v);

// Container expansion (defined with as_args_t below).
template <typename Container>
void push(lua_State* L, const as_args_t<Container>& a);

}  // namespace detail

}  // namespace shd
