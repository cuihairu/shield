// [SHIELD_LUA] Thin Lua C API binding layer (shd) — sol2 replacement.
//
// Design notes (why this exists): sol2 was dropped from the dependency set;
// this header implements the subset of binding semantics the Shield Lua
// surface actually uses, directly on the Lua C API:
//   * registry-ref object/table/function model (copyable, GC-safe),
//   * accessor proxies for `tbl["key"] = v` and read-back,
//   * template C closures for set_function with automatic argument
//     conversion and single-return / multi-return support,
//   * protected calls returning a result object with valid()/error(),
//   * this_state carrier for lambdas that need the raw lua_State*,
//   * stack_object non-owning views, main_thread(), ref_index adoption,
//   * iterable tables (range-for over key/value object pairs),
//   * usertype<T> boxes with methods, __tostring/__eq and no_constructor.
// Deliberately NOT implemented (no call site needs it): coroutine ownership
// (the runtime drives lua_resume directly), inheritance hierarchies.
//
// Error discipline mirrors sol2 closely enough for the Lua tests: argument
// and index conversion failures raise a Lua error inside the protected call
// (the script sees a runtime error; the host sees a !valid() result).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <lua.hpp>
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
// of such a type pushes as userdata with its registered metatable (sol2
// parity for `tbl["k"] = handle` and make_object(state, box)). Specialize
// this in the same header that defines the type; unrelated class types
// (nlohmann::json, view wrappers) deliberately stay unmarked so a missing
// push path fails at compile time instead of pushing a zeroed userdata.
template <typename T>
struct is_usertype_value : std::false_type {};

// Marker for usertypes registered by a foreign binding (sol2 in B1).
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

// ---- this_state -----------------------------------------------------------

// Lightweight carrier handed to set_function lambdas that need the raw
// lua_State* (sol2-compatible shape: constructible from lua_State*).
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

// ---- stack_object -----------------------------------------------------------

// Non-owning view of a value at a fixed stack position (sol2
// stack_reference parity). The index is absolutized on construction; the
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
    return main != nullptr ? main : L;
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
class stack_object;
class protected_function;
class protected_function_result;
class state_view;
class state;

template <typename T, typename... Args>
void new_usertype(state_view sv, const std::string& name, Args&&... args);

// Range wrapper expanding a container as successive call arguments (sol2
// sol::as_args parity); defined after variadic_results.
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
// sol2-created (boxed) userdata. The original sol2 __gc is preserved
// under the key "__sol2_gc" in the metatable.
template <typename T>
inline void install_dual_layout_gc(lua_State* L, const char* type_name) {
    luaL_getmetatable(L, type_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    // Save original __gc under "__sol2_gc"
    lua_getfield(L, -1, "__gc");
    if (!lua_isnil(L, -1)) {
        lua_setfield(L, -2, "__sol2_gc");
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
        // sol2 layout: call original sol2 __gc
        lua_getmetatable(L, 1);
        lua_getfield(L, -1, "__sol2_gc");
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
    if (v)
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
        // Integer binds accept only true Lua integers (sol2 parity: a float
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
        return std::string(s ? s : "", s ? len : 0);
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
    } else {
        const int i = idx++;
        if (!stack_check<D>(L, i)) {
            luaL_error(L, "bad argument #%d (%s expected, got %s)", i,
                       host_type_name<D>(), luaL_typename(L, i));
            // luaL_error longjmps into lua_pcall; the throw below is
            // unreachable and only satisfies the return type.
            throw std::runtime_error("shd::lua: argument conversion failed");
        }
        D v = stack_read<D>(L, i);
        return v;
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
    return std::tuple<Args...>{unpack_arg<Args>(L, idx)...};
}

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

// ---- forward decls --------------------------------------------------------

class object;
class accessor;

// ---- stack_object (full definition) ----------------------------------------

// Non-owning view of a value at a fixed stack position (sol2
// stack_reference parity). The index is absolutized on construction; the
// view is only valid while that stack slot still holds the value.
class stack_object {
public:
    stack_object() : L_(nullptr), idx_(0) {}
    stack_object(lua_State* L, int idx) : L_(L), idx_(lua_absindex(L, idx)) {}
    stack_object(state_view sv, int idx);  // defined after state_view

    lua_State* state() const { return L_; }
    int stack_index() const { return idx_; }
    bool valid() const { return L_ != nullptr; }

    shd::type get_type() const {
        return static_cast<shd::type>(lua_type(L_, idx_));
    }
    bool is_nil() const { return get_type() == shd::type::nil; }

    template <typename T>
    bool is() const {
        using D = std::remove_const_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<D, nil_t>) {
            return is_nil();
        } else if constexpr (std::is_same_v<D, table>) {
            return lua_istable(L_, idx_);
        } else if constexpr (std::is_same_v<D, function>) {
            return lua_isfunction(L_, idx_);
        } else if constexpr (std::is_same_v<D, protected_function>) {
            return lua_isfunction(L_, idx_);
        } else if constexpr (std::is_same_v<D, object>) {
            // object is a registry reference type - any valid
            // userdata/table/function at this stack slot qualifies.
            return lua_type(L_, idx_) != LUA_TNONE &&
                   lua_type(L_, idx_) != LUA_TNIL;
        } else if constexpr (std::is_class_v<D>) {
            return detail::usertype_is<D>(L_, idx_);
        } else {
            return detail::stack_check<D>(L_, idx_);
        }
    }

    template <typename T>
    T as() const {
        using D = std::remove_const_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<D, table>) {
            return table(L_, idx_);
        } else if constexpr (std::is_same_v<D, function>) {
            return function(L_, idx_);
        } else if constexpr (std::is_same_v<D, object>) {
            return object(L_, idx_);
        } else if constexpr (std::is_class_v<D>) {
            if constexpr (std::is_reference_v<T>) {
                // Reference return: bind directly to the userdata on the stack.
                // The caller must ensure the stack slot remains valid.
                // Use detail::usertype_as which handles both value-storage and
                // pointer-box (foreign) layouts.
                return detail::usertype_as<D>(L_, idx_);
            } else {
                return detail::usertype_as<D>(L_, idx_);
            }
        } else {
            return detail::stack_read<D>(L_, idx_);
        }
    }

    operator object() const;

private:
    lua_State* L_;
    int idx_;
};

// Trait: shd reference-ish types held in a Lua value (is<T>/as<T> targets
// that are checked by their lua_type, not by stack_check).
template <typename T>
struct is_ref_type : std::false_type {};
template <>
struct is_ref_type<table> : std::true_type {};
template <>
struct is_ref_type<function> : std::true_type {};
template <>
struct is_ref_type<nil_t> : std::true_type {};

// ---- usertype name registry -------------------------------------------------
//
// shd type identity is metatable identity: is<T>() on a userdata compares its
// metatable against the metatable stored under each registered name. shd
// new_usertype<T> records the name automatically; a usertype registered by a
// different binding surface (the B2 sol2 surface registers ClientContext and
// friends) records itself via register_type_name<T> plus a registry mirror of
// its metatable under that name. The per-state lookup keeps a global name
// list correct across many VMs.

// Global type-name registry with external linkage (avoids per-TU statics in
// the function template). Key is std::type_index, value is vector of names.
inline std::unordered_map<std::type_index, std::vector<std::string>>&
type_name_registry() {
    static std::unordered_map<std::type_index, std::vector<std::string>> reg;
    return reg;
}

template <typename T>
std::vector<std::string>& type_names() {
    return type_name_registry()[std::type_index(typeid(T))];
}
namespace detail {

template <typename D>
bool usertype_is(lua_State* L, const int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA || lua_getmetatable(L, idx) == 0) {
        return false;
    }
    bool ok = false;
    for (const auto& name : type_names<D>()) {
        luaL_getmetatable(L, name.c_str());
        ok = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 1);
        if (ok) break;
    }
    lua_pop(L, 1);  // the value's metatable
    if (!ok) return false;
    // shd-created payloads carry an authoritative type tag in uservalue 1
    // (B1 dual-world seam): sol2 shares one generic metatable across
    // unregistered boxes, so when a tag is present it decides.
    if (lua_getiuservalue(L, idx, 1) == LUA_TSTRING) {
        const char* tag = lua_tostring(L, -1);
        ok = false;
        for (const auto& name : type_names<D>()) {
            if (tag && name == tag) {
                ok = true;
                break;
            }
        }
    }
    lua_pop(L, 1);  // the uservalue (any type)
    return ok;
}

template <typename D>
D& usertype_as(lua_State* L, const int idx) {
    if constexpr (is_foreign_usertype<D>::value) {
        // Pointer-box layout: userdata contains T*
        auto** p = static_cast<D**>(lua_touserdata(L, idx));
        return **p;
    } else {
        // Value storage layout: userdata contains T
        auto* p = static_cast<D*>(lua_touserdata(L, idx));
        return *p;
    }
}

}  // namespace detail

template <typename T>
void register_type_name(const std::string& name) {
    type_names<T>().push_back(name);
}

namespace detail {

// push() for a host type marked as a usertype value: create the userdata and
// attach the metatable registered by new_usertype<T> (first registered name).
// Defined here rather than with the other push overloads because it needs the
// type-name registry.
template <typename T, typename = std::enable_if_t<is_usertype_value<T>::value>>
void push(lua_State* L, const T& v) {
    const auto& names = type_names<T>();
    if (names.empty()) {
        luaL_error(L, "shd: usertype %s is not registered", typeid(T).name());
        throw std::runtime_error("shd: unregistered usertype");  // unreachable
    }
    if constexpr (is_foreign_usertype<T>::value) {
        // Foreign usertype (sol2-registered): use pointer-box layout to match
        // the foreign metatable's __gc expectation. Allocate on heap, store
        // pointer in userdata, attach the foreign metatable.
        auto* heap = new T(v);
        auto** p = static_cast<T**>(lua_newuserdatauv(L, sizeof(T*), 0));
        *p = heap;
        luaL_getmetatable(L, names.front().c_str());
        lua_setmetatable(L, -2);
        // No uservalue tag: is_shd_raw_userdata will return false, so the
        // foreign path in sol_box_context_marker will be taken (which uses
        // sol::object to read the pointer-box layout).
    } else {
        // Native shd usertype: value storage with type-name tag in uservalue 1.
        auto* p = static_cast<T*>(lua_newuserdatauv(L, sizeof(T), 1));
        new (p) T(v);
        luaL_getmetatable(L, names.front().c_str());
        lua_setmetatable(L, -2);
        lua_pushlstring(L, names.front().data(), names.front().size());
        lua_setiuservalue(L, -2, 1);
    }
}

// Layout tag for the B1 dual-world seam: shd-created usertype userdata
// carries its type name in uservalue 1 with the payload raw (T in place),
// while sol2-created boxes store the payload in a sol-internal box layout
// with no uservalues. The mirrored metatables give identity only (sol2 uses
// one generic metatable for unregistered boxes), so the tag distinguishes
// both layout and exact type.
inline bool is_shd_raw_userdata(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA) return false;
    const int t = lua_getiuservalue(L, idx, 1);
    lua_pop(L, 1);
    return t == LUA_TSTRING;
}

}  // namespace detail

// ---- table / object (registry references) ---------------------------------

namespace detail {

// Shared registry-reference holder.
class ref_base {
public:
    ref_base() : L_(nullptr), ref_(LUA_NOREF) {}
    ref_base(lua_State* L, int stack_idx) : L_(L) {
        lua_pushvalue(L, stack_idx);
        ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    // From an existing registry ref: re-refs the value so source and copy
    // own independent entries (see ref_index_tag).
    ref_base(lua_State* L, ref_index_tag r) : L_(L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, r.ref);
        ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    ref_base(const ref_base& o) : L_(o.L_), ref_(LUA_NOREF) {
        if (L_ && o.ref_ != LUA_NOREF) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, o.ref_);
            ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    }
    ref_base& operator=(const ref_base& o) {
        if (this != &o) {
            release();
            L_ = o.L_;
            if (L_ && o.ref_ != LUA_NOREF) {
                lua_rawgeti(L_, LUA_REGISTRYINDEX, o.ref_);
                ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
            } else {
                ref_ = LUA_NOREF;
            }
        }
        return *this;
    }
    ref_base(ref_base&& o) noexcept : L_(o.L_), ref_(o.ref_) {
        o.L_ = nullptr;
        o.ref_ = LUA_NOREF;
    }
    ref_base& operator=(ref_base&& o) noexcept {
        if (this != &o) {
            release();
            L_ = o.L_;
            ref_ = o.ref_;
            o.L_ = nullptr;
            o.ref_ = LUA_NOREF;
        }
        return *this;
    }
    virtual ~ref_base() { release(); }

    lua_State* state() const { return L_; }
    int ref_index() const { return ref_; }
    // sol2 naming aliases used by runtime/service call sites.
    lua_State* lua_state() const { return L_; }
    int registry_index() const { return ref_; }
    // LUA_REFNIL is what luaL_ref yields for a nil value: the reference
    // exists but denotes nil, so valid() must reject it (sol2 parity:
    // `lua["missing"].valid()` is false).
    bool valid() const {
        return L_ != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL;
    }

    // Drop the reference without luaL_unref (sol2 abandon parity): the
    // caller may not be on the VM's owner thread, so the registry entry is
    // deliberately leaked — it dies with the VM.
    void abandon() {
        L_ = nullptr;
        ref_ = LUA_NOREF;
    }

    // Push the referenced value onto the stack; returns stack index.
    int push() const {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);
        return lua_gettop(L_);
    }

private:
    void release() {
        if (L_ && ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
        ref_ = LUA_NOREF;
    }

protected:
    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};

}  // namespace detail

// Generic referenced Lua value. Knows its shd::type; converts to host types
// with as<T>() and queries with is<T>().
class object : public detail::ref_base {
public:
    object() = default;
    object(lua_State* L, int idx) : detail::ref_base(L, idx) {}
    object(lua_State* L, ref_index_tag r) : detail::ref_base(L, r) {}
    // nil literal: an invalid reference reads as nil everywhere (is_nil(),
    // nil-on-push), matching sol::object(sol::lua_nil) semantics.
    object(nil_t) {}
    // Reference-type values convert to object implicitly (sol2 parity): a
    // table/function/protected_function ref becomes an owned object ref.
    // Reference-type conversions (defined after all ref types complete).
    object(const table& t);
    object(const function& f);
    object(const protected_function& f);
    object(const stack_object& so);

    // Construct from any host value.
    template <typename T, typename = std::enable_if_t<!std::is_base_of_v<
                              detail::ref_base, std::decay_t<T>>>>
    object(lua_State* L, T&& v)
        : detail::ref_base(L, make_tmp(L, std::forward<T>(v))) {}

    shd::type get_type() const {
        if (!valid()) return shd::type::nil;
        const int i = push();
        const shd::type t = static_cast<shd::type>(lua_type(L_, i));
        lua_pop(L_, 1);
        return t;
    }

    bool is_nil() const { return get_type() == shd::type::nil; }

    bool operator==(nil_t) const { return is_nil(); }
    bool operator!=(nil_t) const { return !is_nil(); }

    template <typename T>
    bool is() const {
        // Normalize reference/const-qualified targets before dispatching:
        // is_class_v is false on reference types and would misroute usertype
        // reads like as<const Box&>() into the primitive stack_read branch.
        using D = std::remove_const_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<D, nil_t>) {
            return is_nil();
        } else if constexpr (std::is_same_v<D, table>) {
            if (!valid()) return false;
            const int i = push();
            const bool ok = lua_istable(L_, i) != 0;
            lua_pop(L_, 1);
            return ok;
        } else if constexpr (std::is_same_v<D, function>) {
            if (!valid()) return false;
            const int i = push();
            const bool ok = lua_isfunction(L_, i) != 0;
            lua_pop(L_, 1);
            return ok;
        } else if constexpr (std::is_same_v<D, protected_function>) {
            if (!valid()) return false;
            const int i = push();
            const bool ok = lua_isfunction(L_, i) != 0;
            lua_pop(L_, 1);
            return ok;
        } else if constexpr (std::is_same_v<D, object>) {
            // object is a registry reference type, not a usertype.
            if (!valid()) return false;
            const int i = push();
            const bool ok = true;  // Any valid registry reference is an object.
            lua_pop(L_, 1);
            return ok;
        } else if constexpr (std::is_class_v<D> &&
                             !std::is_same_v<D, std::string>) {
            // Registered usertype: metatable identity via the name registry
            // (shared with stack_object, see detail::usertype_is).
            if (!valid()) return false;
            const int i = push();
            const bool ok = detail::usertype_is<D>(L_, i);
            lua_pop(L_, 1);
            return ok;
        } else {
            if (!valid()) return false;
            const int i = push();
            const bool ok = detail::stack_check<D>(L_, i);
            lua_pop(L_, 1);
            return ok;
        }
    }

    template <typename T>
    T as() const {
        // Same reference normalization as is<>: see the note there.
        using D0 = std::remove_const_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<D0, table> ||
                      std::is_same_v<D0, function>) {
            // Same reference reasoning as the primitive branch below: these
            // views are built locally, so a reference target would dangle.
            static_assert(!std::is_reference_v<T>,
                          "shd::object::as<T>(): table/function views must be "
                          "taken by value, not by reference");
            const int i = push();
            if (!(std::is_same_v<T, table> ? lua_istable(L_, i)
                                           : lua_isfunction(L_, i))) {
                luaL_error(L_, "bad cast (%s expected, got %s)",
                           std::is_same_v<T, table> ? "table" : "function",
                           luaL_typename(L_, i));
                throw std::runtime_error("shd::lua: bad cast");  // unreachable
            }
            T v(L_, i);
            lua_pop(L_, 1);
            return v;
        } else if constexpr (std::is_same_v<D0, protected_function>) {
            const int i = push();
            protected_function pf(L_, i);
            lua_pop(L_, 1);
            return pf;
        } else if constexpr (std::is_same_v<D0, object>) {
            // object is a registry reference type, not a usertype. Create a new
            // object reference from the stack slot (same pattern as
            // table/function).
            static_assert(!std::is_reference_v<T>,
                          "shd::object::as<object>(): object views must be "
                          "taken by value, not by reference");
            const int i = push();
            object o(L_, i);
            lua_pop(L_, 1);
            return o;
        } else if constexpr (std::is_class_v<D0> &&
                             !std::is_same_v<D0, std::string>) {
            // Registered usertype: read the userdata payload. A non-userdata
            // or foreign-usertype value here is caller error (is<> guards
            // the runtime call sites); sol2 parity treats it the same way.
            const int i = push();
            if constexpr (std::is_reference_v<T>) {
                // Reference return: bind directly to the userdata on the stack
                // (kept alive by the registry reference held by this object).
                // Use detail::usertype_as which handles both value-storage and
                // pointer-box (foreign) layouts.
                using U = std::remove_reference_t<T>;
                U& ref = detail::usertype_as<U>(L_, i);
                lua_pop(L_, 1);
                return ref;
            } else {
                D0 v = detail::usertype_as<D0>(L_, i);
                lua_pop(L_, 1);
                return v;
            }
        } else {
            // Primitives come off the stack by value: a reference target here
            // would bind to the local below (and, for strings, into Lua's
            // interned storage that lua_pop invalidates), so reject it at
            // compile time instead of handing back a dangling reference.
            static_assert(!std::is_reference_v<T>,
                          "shd::object::as<T>(): primitives must be read by "
                          "value, not by reference");
            const int i = push();
            if (!detail::stack_check<D0>(L_, i)) {
                luaL_error(L_, "bad cast (%s expected, got %s)",
                           detail::host_type_name<D0>(), luaL_typename(L_, i));
                // luaL_error longjmps into the enclosing pcall; unreachable.
                throw std::runtime_error("shd::lua: bad cast");
            }
            D0 v = detail::stack_read<D0>(L_, i);
            lua_pop(L_, 1);
            return v;
        }
    }

    // Element access on a table-valued object (defined after accessor).
    accessor operator[](const std::string& k) const;
    accessor operator[](const char* k) const;

    // Reference views of the same value (sol2 parity): a generic object
    // converts to the reference type its value actually carries. Unchecked,
    // like sol2 — the caller asked for the view explicitly. Out-of-line: the
    // reference classes complete after this one.
    operator table() const;
    operator function() const;
    operator protected_function() const;

    // Implicit conversion to the host types stack_read supports (sol2
    // parity for `std::string s = obj;` style call sites).
    template <typename T,
              typename = std::enable_if_t<detail::is_stack<T>::value>>
    operator T() const {
        return as<T>();
    }

private:
    template <typename T>
    static int make_tmp(lua_State* L, T&& v) {
        detail::push(L, std::forward<T>(v));
        return lua_gettop(L);  // ref_base takes the idx from the top
    }
};

namespace detail {
inline void push(lua_State* L, const object& o) {
    if (o.valid()) {
        o.push();
    } else {
        lua_pushnil(L);
    }
}
}  // namespace detail

inline stack_object::operator object() const { return object(L_, idx_); }

// Referenced Lua function (callable via protected call).
class function : public detail::ref_base {
public:
    function() = default;
    function(lua_State* L, int idx) : detail::ref_base(L, idx) {}
    function(lua_State* L, ref_index_tag r) : detail::ref_base(L, r) {}
    // nil literal: an invalid function (callers check valid() before use).
    function(nil_t) {}
    // Cross-conversion (sol2 parity): protected_function and function share
    // the same underlying registry reference. Defined after
    // protected_function completes.
    function(const protected_function& pf);
    // Adopt whatever the object references (sol2 function-from-object
    // parity; a non-function ref errors at call time, not construction).
    function(const object& o) : detail::ref_base(o) {}

    // Protected call with variadic args; returns the first result or nil.
    // Argument count is measured from the stack (not sizeof...(Args)):
    // an as_args argument contributes one value per container element.
    template <typename... Args>
    object call(Args&&... args) const {
        lua_State* L = state();
        push();  // the callable
        const int base = lua_gettop(L) - 1;
        (detail::push(L, std::forward<Args>(args)), ...);
        const int nargs = lua_gettop(L) - base - 1;
        const int status = lua_pcall(L, nargs, 1, 0);
        if (status != LUA_OK) {
            lua_pop(L, 1);  // error message
            return object();
        }
        object r(L, -1);
        lua_pop(L, 1);
        return r;
    }

    template <typename... Args>
    object operator()(Args&&... args) const {
        return call(std::forward<Args>(args)...);
    }
};

namespace detail {
inline void push(lua_State* L, const function& f) {
    if (f.valid()) {
        f.push();
    } else {
        lua_pushnil(L);
    }
}
}  // namespace detail

// Key accessor proxy: `tbl["k"] = v` writes through; reading converts. Holds
// its own registry reference to the containing table so it stays valid even
// Reference equality through Lua: two objects are equal when lua_rawequal
// holds for the values they reference (sol2 object==object parity).
inline bool operator==(const object& a, const object& b) {
    if (!a.valid() || !b.valid()) return !a.valid() && !b.valid();
    const int i = a.push();
    const int j = b.push();
    const bool eq = lua_rawequal(a.state(), i, j) != 0;
    lua_pop(a.state(), 2);
    return eq;
}
inline bool operator!=(const object& a, const object& b) { return !(a == b); }

// Key accessor proxy: `tbl["k"] = v` writes through; reading converts. Holds
// its own registry reference to the containing table so it stays valid even
// when the table proxy it was created from goes out of scope.
class accessor {
public:
    accessor(lua_State* L, const detail::ref_base& t,
             std::variant<std::string, std::int64_t> key)
        : owner_(t), key_(std::move(key)) {
        (void)L;
    }

    template <typename T>
    accessor& operator=(T&& v) {
        lua_State* L = owner_.state();
        lua_rawgeti(L, LUA_REGISTRYINDEX, owner_.ref_index());
        push_key(L);
        detail::push(L, std::forward<T>(v));
        lua_settable(L, -3);
        lua_pop(L, 1);
        return *this;
    }

    // Read as object (no conversion error; type checked by caller).
    object value() const {
        lua_State* L = owner_.state();
        lua_rawgeti(L, LUA_REGISTRYINDEX, owner_.ref_index());
        push_key(L);
        lua_gettable(L, -2);
        object o(L, -1);
        lua_pop(L, 2);
        return o;
    }

    bool valid() const { return value().valid(); }
    shd::type get_type() const { return value().get_type(); }

    // Read with a fallback (sol2 accessor::get_or parity): nil/missing keys
    // yield the default; present values convert.
    template <typename T>
    T get_or(T def) const {
        object v = value();
        if (!v.valid() || v.is_nil()) return def;
        return v.as<T>();
    }

    template <typename T>
    T as() const {
        return value().as<T>();
    }

    // sol2 accessor parity alias.
    template <typename T>
    T get() const {
        return value().as<T>();
    }

    bool operator==(nil_t) const { return value().is_nil(); }
    bool operator!=(nil_t) const { return !value().is_nil(); }

    template <typename T>
    bool is() const {
        return value().is<T>();
    }

    operator object() const { return value(); }
    operator table() const;  // defined after table
    operator function() const {
        object v = value();
        if (!v.valid()) return function();
        return function(v.state(), v.push());
    }
    // Out-of-line: protected_function is defined later in this header.
    operator protected_function() const;

    // Implicit read to host stack types (bool/string/number; sol2 parity for
    // `bool b = lua["flag"];` call sites). Reference/const-qualified targets
    // normalize inside value().as<T>.
    template <typename T,
              typename = std::enable_if_t<detail::is_stack<T>::value>>
    operator T() const {
        return value().as<T>();
    }

    // Chained element access on a table-valued accessor.
    accessor operator[](const std::string& k) const {
        object v = value();
        return accessor(v.state(), v,
                        std::variant<std::string, std::int64_t>(k));
    }
    accessor operator[](const char* k) const {
        object v = value();
        return accessor(
            v.state(), v,
            std::variant<std::string, std::int64_t>(std::string(k)));
    }
    accessor operator[](std::int64_t k) const {
        object v = value();
        return accessor(v.state(), v,
                        std::variant<std::string, std::int64_t>(k));
    }

private:
    void push_key(lua_State* L) const {
        if (key_.index() == 0) {
            const auto& s = std::get<0>(key_);
            lua_pushlstring(L, s.data(), s.size());
        } else {
            lua_pushinteger(L, std::get<1>(key_));
        }
    }

    detail::ref_base owner_;
    std::variant<std::string, std::int64_t> key_;
};

inline accessor object::operator[](const std::string& k) const {
    return accessor(state(), *this, std::variant<std::string, std::int64_t>(k));
}
inline accessor object::operator[](const char* k) const {
    return accessor(state(), *this,
                    std::variant<std::string, std::int64_t>(std::string(k)));
}

class table : public detail::ref_base {
public:
    table() = default;
    table(lua_State* L, int idx) : detail::ref_base(L, idx) {}
    table(lua_State* L, ref_index_tag r) : detail::ref_base(L, r) {}
    // nil literal: an invalid table (callers check valid() before use).
    table(nil_t) {}

    lua_State* lua_state() const { return state(); }

    shd::type get_type() const {
        if (!valid()) return shd::type::nil;
        const int i = push();
        const shd::type t = static_cast<shd::type>(lua_type(L_, i));
        lua_pop(L_, 1);
        return t;
    }

    bool valid_table() const { return get_type() == shd::type::table; }

    // -- element access ----------------------------------------------------
    accessor operator[](const char* k) const {
        return accessor(
            state(), *this,
            std::variant<std::string, std::int64_t>(std::string(k)));
    }
    accessor operator[](const std::string& k) const {
        return accessor(state(), *this,
                        std::variant<std::string, std::int64_t>(k));
    }
    accessor operator[](std::int64_t k) const {
        return accessor(state(), *this,
                        std::variant<std::string, std::int64_t>(k));
    }
    // Object-valued key (LuaPack decode map path): read/write through a
    // canonicalized key object.
    object raw_get(const object& k) const {
        const int i = push();
        k.push();
        lua_rawget(L_, i);
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
    }
    template <typename T>
    void raw_set(const object& k, T&& v) {
        const int i = push();
        k.push();
        detail::push(L_, std::forward<T>(v));
        lua_rawset(L_, i);
    }

    object raw_get(const std::string& k) const {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        lua_rawget(L_, i);
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
    }
    // Convert-on-read form (sol2 raw_get<T> parity): the proxy conversion to
    // table/function silently yields an invalid reference for a wrong-typed
    // entry, which is why the timer test reads create_table members this way.
    template <typename T>
    T raw_get(const std::string& k) const {
        return raw_get(k).as<T>();
    }
    object raw_get(std::int64_t k) const {
        const int i = push();
        lua_pushinteger(L_, k);
        lua_rawget(L_, i);
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
    }

    template <typename T>
    T get(const std::string& k) const {
        return raw_get(k).as<T>();
    }
    template <typename T>
    T get(std::int64_t k) const {
        return raw_get(k).as<T>();
    }
    template <typename T>
    T get_or(const std::string& k, T def) const {
        object o = raw_get(k);
        if (o.is_nil()) return def;
        return o.as<T>();
    }

    template <typename T>
    void set(const std::string& k, T&& v) {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        detail::push(L_, std::forward<T>(v));
        lua_settable(L_, i);
    }
    template <typename T>
    void raw_set(const std::string& k, T&& v) {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        detail::push(L_, std::forward<T>(v));
        lua_rawset(L_, i);
    }
    template <typename T>
    void raw_set(std::int64_t k, T&& v) {
        const int i = push();
        lua_pushinteger(L_, k);
        detail::push(L_, std::forward<T>(v));
        lua_rawset(L_, i);
    }
    template <typename T>
    void set(std::int64_t k, T&& v) {
        const int i = push();
        lua_pushinteger(L_, k);
        detail::push(L_, std::forward<T>(v));
        lua_settable(L_, i);
    }

    // -- function registration ---------------------------------------------
    template <typename F>
    void set_function(const std::string& name, F&& f) {
        const int i = push();
        lua_pushlstring(L_, name.data(), name.size());
        push_closure(std::forward<F>(f));
        lua_settable(L_, i);
    }

    // Create a fresh table nested in this one.
    table create_table(const std::string& name) {
        const int i = push();
        lua_newtable(L_);
        table t(L_, -1);
        lua_pushlstring(L_, name.data(), name.size());
        lua_pushvalue(L_, -2);
        lua_settable(L_, i);
        lua_pop(L_, 1);
        return t;
    }
    table create_table(std::int64_t idx_key) {
        const int i = push();
        lua_newtable(L_);
        table t(L_, -1);
        lua_pushinteger(L_, idx_key);
        lua_pushvalue(L_, -2);
        lua_settable(L_, i);
        lua_pop(L_, 1);
        return t;
    }

    // Size of the sequence part.
    std::size_t size() const {
        const int i = push();
        const lua_Integer n = lua_rawlen(L_, i);
        lua_pop(L_, 1);
        return static_cast<std::size_t>(n);
    }

    // Append a value at the end of the sequence part (sol2 add parity).
    void add(const object& v) const {
        const int i = push();
        v.push();
        lua_seti(L_, i, static_cast<lua_Integer>(lua_rawlen(L_, i)) + 1);
        lua_pop(L_, 1);
    }

    // Iterate key/value as objects (insertion order not preserved; the call
    // sites that need order keep their own integer indexing).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        const int i = push();
        lua_pushnil(L_);
        while (lua_next(L_, i) != 0) {
            object k(L_, -2);
            object v(L_, -1);
            fn(k, v);
            lua_pop(L_, 1);  // keep key for next iteration
        }
        lua_pop(L_, 1);
    }

    // Range-for support: `for (const auto& [k, v] : t.as<shd::table>())`.
    // Traversal order follows lua_next (not insertion order). Defined after
    // table_iterator (the class is declared later in this header).
    table_iterator begin() const;
    table_iterator end() const;

private:
    // Closure trampoline: upvalue 1 carries a std::function dispatcher.
    template <typename F>
    void push_closure(F&& f);
};

// Range-for iterator over a table's key/value pairs (sol2 iterates tables
// as pairs of objects; order follows lua_next traversal). Owns its registry
// reference to the table, so the iteration survives the table proxy going
// out of scope — but not mutation of the table during the loop.
class table_iterator {
public:
    using value_type = std::pair<object, object>;

    table_iterator() : L_(nullptr), at_end_(true) {}
    table_iterator(lua_State* L, const detail::ref_base& owner)
        : owner_(owner), L_(L), at_end_(false) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, owner_.ref_index());
        lua_pushnil(L);
        if (lua_next(L, -2) == 0) {
            at_end_ = true;
            lua_pop(L, 1);  // the table
            return;
        }
        key_ = object(L, -2);
        value_ = object(L, -1);
        lua_pop(L, 3);  // value, key, table
    }

    value_type operator*() const { return {key_, value_}; }

    table_iterator& operator++() {
        if (at_end_) return *this;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, owner_.ref_index());
        detail::push(L_, key_);
        if (lua_next(L_, -2) == 0) {
            at_end_ = true;
            lua_pop(L_, 1);
            return *this;
        }
        key_ = object(L_, -2);
        value_ = object(L_, -1);
        lua_pop(L_, 3);
        return *this;
    }

    bool operator!=(const table_iterator& /*end*/) const { return !at_end_; }
    bool operator==(const table_iterator& /*end*/) const { return at_end_; }

private:
    detail::ref_base owner_;
    lua_State* L_ = nullptr;
    bool at_end_ = true;
    object key_;
    object value_;
};

namespace detail {
inline void push(lua_State* L, const table& t) {
    if (t.valid()) {
        t.push();
    } else {
        lua_pushnil(L);
    }
}
inline void push(lua_State* L, const accessor& a) { push(L, a.value()); }
}  // namespace detail

inline table_iterator table::begin() const {
    return table_iterator(state(), *this);
}
inline table_iterator table::end() const { return table_iterator(); }

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
// (sol2 lua_CFunction parity). The thunk's exception guard still applies.
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
    // to Lua errors like sol2 does. The catch handlers COMPLETE (freeing the
    // exception objects) before luaL_error longjmps out — calling lua_error
    // inside a handler would skip the exception cleanup and leak it.
    // msgbuf is a POD array on purpose: longjmp may skip its "destruction",
    // and a POD has none.
    char msgbuf[512] = {0};
    int kind = 0;
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
}

// Build the dispatcher for a callable with explicit signature.
template <typename R, typename... Args, typename F>
std::function<int(lua_State*)> make_dispatcher(F&& f) {
    return [f](lua_State* L) -> int {
        int idx = 1;
        auto args = unpack_all<std::decay_t<Args>...>(
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
    new (store)
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

// ---- state ----------------------------------------------------------------

// Library masks for open_libraries (sol2-compatible subset).
namespace lib {
inline constexpr int base = 1 << 0;
inline constexpr int package = 1 << 1;
inline constexpr int coroutine = 1 << 2;
inline constexpr int string = 1 << 3;
inline constexpr int os = 1 << 4;
inline constexpr int math = 1 << 5;
inline constexpr int table = 1 << 6;
inline constexpr int debug = 1 << 7;
inline constexpr int bit32 = 1 << 8;
inline constexpr int io = 1 << 9;
inline constexpr int ffi = 1 << 10;
inline constexpr int jit = 1 << 11;
inline constexpr int count = 12;
}  // namespace lib

// Loading/script error container (sol2-compatible shape: what()).
class error {
public:
    error() = default;
    explicit error(std::string msg) : msg_(std::move(msg)) {}
    // const char* (not std::string) so `fprintf(..., "%s", e.what())` keeps
    // working exactly as it did under sol2.
    const char* what() const noexcept { return msg_.c_str(); }
    explicit operator bool() const { return !msg_.empty(); }

private:
    std::string msg_;
};

// Result of a protected call. Owns the returned values on the Lua stack
// (LUA_MULTRET): `get(i)` fetches the i-th result, `return_count()` their
// number, and the destructor pops them. Move-only — moving steals the stack
// obligation so the values survive being carried up to the caller.
class protected_function_result {
public:
    protected_function_result() = default;
    protected_function_result(lua_State* L, int status, int base, int count)
        : L_(L), status_(status), base_(base), count_(count) {}

    protected_function_result(const protected_function_result&) = delete;
    protected_function_result& operator=(const protected_function_result&) =
        delete;

    protected_function_result(protected_function_result&& o) noexcept
        : L_(o.L_), status_(o.status_), base_(o.base_), count_(o.count_) {
        o.L_ = nullptr;
        o.count_ = 0;
    }
    protected_function_result& operator=(
        protected_function_result&& o) noexcept {
        if (this != &o) {
            pop_owned();
            L_ = o.L_;
            status_ = o.status_;
            base_ = o.base_;
            count_ = o.count_;
            o.L_ = nullptr;
            o.count_ = 0;
        }
        return *this;
    }
    ~protected_function_result() { pop_owned(); }

    bool valid() const { return status_ == LUA_OK; }
    int status() const { return status_; }
    int return_count() const { return count_; }

    // i-th returned value (0-based; results occupy base_+1 .. base_+count).
    // On a failed call, index 0 is the error message object. The returned
    // object holds its own registry reference, so it stays valid after this
    // result pops the stack.
    object get(int i) const { return object(L_, base_ + 1 + i); }
    template <typename T>
    T get(int i) const {
        using D = std::remove_const_t<std::remove_reference_t<T>>;
        const int idx = base_ + 1 + i;
        if constexpr (std::is_same_v<D, table> || std::is_same_v<D, function>) {
            return D(L_, idx);
        } else if constexpr (std::is_same_v<D, object>) {
            return object(L_, idx);
        } else if constexpr (std::is_class_v<D>) {
            auto* p = static_cast<D*>(lua_touserdata(L_, idx));
            return *p;
        } else {
            return detail::stack_read<D>(L_, idx);
        }
    }
    object get() const { return get(0); }
    template <typename T>
    T get() const {
        return get<T>(0);
    }

    error get_error() const {
        if (valid()) return error();
        if (L_ == nullptr || count_ <= 0) {
            return error(std::string("unknown error"));
        }
        const int idx = base_ + 1;
        if (lua_type(L_, idx) == LUA_TSTRING) {
            return error(detail::stack_read<std::string>(L_, idx));
        }
        return error(std::string("unknown error"));
    }
    operator error() const { return get_error(); }
    operator object() const { return get(0); }

private:
    void pop_owned() {
        if (L_ != nullptr && count_ > 0) lua_pop(L_, count_);
        count_ = 0;
    }

    lua_State* L_ = nullptr;
    int status_ = LUA_ERRRUN;
    int base_ = 0;
    int count_ = 0;
};

class protected_function : public detail::ref_base {
public:
    protected_function() = default;
    // nil literal: an invalid function (call() reports a failed result).
    protected_function(nil_t) {}
    explicit protected_function(const object& o) {
        if (o.valid()) {
            L_ = o.state();
            o.push();
            ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    }
    // Cross-conversion (sol2 parity): a plain function reference upgrades to
    // a protected call wrapper.
    protected_function(const function& f);
    protected_function(lua_State* L, int idx) : detail::ref_base(L, idx) {}
    protected_function(lua_State* L, ref_index_tag r)
        : detail::ref_base(L, r) {}

    template <typename... Args>
    protected_function_result call(Args&&... args) const {
        lua_State* L = state();
        if (!valid()) {
            return protected_function_result(L, LUA_ERRRUN, 0, 0);
        }
        push();                              // the callable
        const int base = lua_gettop(L) - 1;  // slot below the callable
        (detail::push(L, std::forward<Args>(args)), ...);
        const int nargs = lua_gettop(L) - base - 1;
        const int status = lua_pcall(L, nargs, LUA_MULTRET, 0);
        const int count = lua_gettop(L) - base;
        return protected_function_result(L, status, base, count);
    }

    template <typename... Args>
    protected_function_result operator()(Args&&... args) const {
        return call(std::forward<Args>(args)...);
    }
};

// Reference-view conversions (out-of-line: those classes complete after
// object/accessor).
inline object::operator table() const { return as<table>(); }
inline object::operator function() const {
    if (!valid()) return function();
    const int i = push();
    function f(L_, i);
    lua_pop(L_, 1);
    return f;
}
inline object::operator protected_function() const {
    if (!valid()) return protected_function();
    const int i = push();
    protected_function f(L_, i);
    lua_pop(L_, 1);
    return f;
}
inline accessor::operator table() const { return value().as<table>(); }
inline accessor::operator protected_function() const {
    return protected_function(value());
}

// Cross-type reference conversions: each re-refs the source value so the
// copies own independent registry entries.
inline object::object(const table& t) {
    if (t.valid()) {
        L_ = t.state();
        t.push();
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}
inline object::object(const function& f) {
    if (f.valid()) {
        L_ = f.state();
        f.push();
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}
inline object::object(const protected_function& f) {
    if (f.valid()) {
        L_ = f.state();
        f.push();
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}
inline object::object(const stack_object& so) {
    if (so.valid()) {
        L_ = so.state();
        lua_pushvalue(L_, so.stack_index());
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}
inline function::function(const protected_function& pf) {
    if (pf.valid()) {
        L_ = pf.state();
        pf.push();
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}
inline protected_function::protected_function(const function& f) {
    if (f.valid()) {
        L_ = f.state();
        f.push();
        ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
}

// Multi-return carrier for lambdas returning several values.
class variadic_results {
public:
    variadic_results() = default;
    explicit variadic_results(object v) { parts_.push_back(std::move(v)); }
    variadic_results& push_back(object v) {
        parts_.push_back(std::move(v));
        return *this;
    }
    const std::vector<object>& parts() const { return parts_; }

private:
    std::vector<object> parts_;
};

namespace detail {
inline int push_result(lua_State* L, int base, variadic_results&& v) {
    (void)base;
    for (const object& o : v.parts()) push(L, o);
    return static_cast<int>(v.parts().size());
}
}  // namespace detail

// Range wrapper: expands a container as successive call arguments (sol2
// sol::as_args parity). The call machinery counts pushed values (not
// argument packs), so an as_args argument contributes container.size()
// arguments to the protected call.
template <typename Container>
struct as_args_t {
    const Container& container;
};
template <typename Container>
as_args_t<Container> as_args(const Container& c) {
    return as_args_t<Container>{c};
}

namespace detail {
template <typename Container>
void push(lua_State* L, const as_args_t<Container>& a) {
    for (const auto& element : a.container) push(L, element);
}
}  // namespace detail

// Result of state_view::load: holds the compiled chunk (or the error
// message) as an owned object plus the load status (sol2 load_result
// parity: valid() / convertible to error / to protected_function).
class load_result {
public:
    load_result() = default;
    load_result(lua_State* L, int status, int slot) : status_(status) {
        if (L != nullptr) value_ = object(L, slot);
    }

    bool valid() const { return status_ == LUA_OK; }
    int status() const { return status_; }

    operator error() const {
        if (valid() || !value_.valid()) return error();
        return error(value_.as<std::string>());
    }
    operator protected_function() const { return protected_function(value_); }
    operator object() const { return value_; }

private:
    object value_;
    int status_ = LUA_ERRSYNTAX;
};

// ---- state / state_view ---------------------------------------------------

class state_view {
public:
    explicit state_view(lua_State* L) : L_(L) {}

    lua_State* lua_state() const { return L_; }

    table create_table() const {
        lua_newtable(L_);
        table t(L_, -1);
        lua_pop(L_, 1);
        return t;
    }

    table globals() const {
        lua_pushglobaltable(L_);
        table t(L_, -1);
        lua_pop(L_, 1);
        return t;
    }

    object get(const std::string& k) const {
        lua_pushglobaltable(L_);
        lua_pushlstring(L_, k.data(), k.size());
        lua_gettable(L_, -2);
        object o(L_, -1);
        lua_pop(L_, 2);
        return o;
    }

    // Global element access; the accessor converts to object on read and
    // writes through on assignment: `state["name"] = value;`
    accessor operator[](const char* k) const {
        table g = globals();
        return accessor(
            L_, g, std::variant<std::string, std::int64_t>(std::string(k)));
    }
    accessor operator[](const std::string& k) const {
        table g = globals();
        return accessor(L_, g, std::variant<std::string, std::int64_t>(k));
    }
    accessor operator[](std::string_view k) const {
        return operator[](std::string(k));
    }

    // Run a file; returns its first return value (nil on failure).
    object script_file(const std::string& path) const {
        const int status = luaL_loadfile(L_, path.c_str());
        if (status != LUA_OK) {
            lua_pop(L_, 1);  // error message
            return object();
        }
        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            lua_pop(L_, 1);
            return object();
        }
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
    }

    // -- protected script loading/running ------------------------------------
    bool do_string(const std::string& code) const {
        return script(code, "string").valid();
    }

    protected_function_result script(
        const std::string& code,
        const std::string& chunkname = "script") const {
        return load_protected(chunkname, code);
    }

    // Load a file's contents as a chunk (returns function result).
    protected_function_result load_file(const std::string& path) const {
        return load_protected(path, std::string());
    }

    // Compile a chunk without running it (sol2 state::load parity). The
    // result converts to protected_function for execution or to error on a
    // syntax failure.
    load_result load(const std::string& code,
                     const std::string& chunkname = "string") const {
        const int status =
            luaL_loadbuffer(L_, code.data(), code.size(), chunkname.c_str());
        return load_result(L_, status, -1);
    }

    // -- library loading -----------------------------------------------------
    void open_libraries(int libs) const {
        if (libs & lib::base) {
            luaL_requiref(L_, "_G", luaopen_base, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::package) {
            luaL_requiref(L_, LUA_LOADLIBNAME, luaopen_package, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::coroutine) {
            luaL_requiref(L_, LUA_COLIBNAME, luaopen_coroutine, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::string) {
            luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::os) {
            luaL_requiref(L_, LUA_OSLIBNAME, luaopen_os, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::math) {
            luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::io) {
            luaL_requiref(L_, LUA_IOLIBNAME, luaopen_io, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::table) {
            luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::debug) {
            luaL_requiref(L_, LUA_DBLIBNAME, luaopen_debug, 1);
            lua_pop(L_, 1);
        }
    }

    // sol2-style variadic form: open_libraries(lib::base, lib::string, ...).
    // A single-argument call resolves to the int overload above.
    template <typename... Libs>
    void open_libraries(Libs... libs) const {
        int mask = 0;
        ((mask |= static_cast<int>(libs)), ...);
        open_libraries(mask);
    }

    // sol2 state::new_usertype parity (forwards to the free function).
    template <typename T, typename... Args>
    void new_usertype(const std::string& name, Args&&... args) const {
        shd::new_usertype<T>(*this, name, std::forward<Args>(args)...);
    }

protected:
    lua_State* L_ = nullptr;

    // luaL_loadbuffer/luaL_loadfile + pcall in one protected step. When
    // `code` is empty the first argument is a file path.
    protected_function_result load_protected(const std::string& chunkname,
                                             const std::string& code) const {
        const int status = code.empty()
                               ? luaL_loadfile(L_, chunkname.c_str())
                               : luaL_loadbuffer(L_, code.data(), code.size(),
                                                 chunkname.c_str());
        if (status != LUA_OK) {
            // The load error message sits on top of the stack.
            const int base = lua_gettop(L_) - 1;
            return protected_function_result(L_, status, base, 1);
        }
        const int base = lua_gettop(L_) - 1;  // slot below the chunk
        const int run = lua_pcall(L_, 0, LUA_MULTRET, 0);
        const int count = lua_gettop(L_) - base;
        return protected_function_result(L_, run, base, count);
    }
};

class state : public state_view {
public:
    state() : state_view(luaL_newstate()), owned_(true) {
        if (!L_)
            throw std::runtime_error("shd::state: lua state allocation failed");
    }
    explicit state(lua_State* existing) : state_view(existing), owned_(false) {}
    // sol2 parity: a state converts to its raw lua_State*.
    operator lua_State*() const { return L_; }
    state(const state&) = delete;
    state& operator=(const state&) = delete;
    state(state&& o) noexcept : state_view(o.L_), owned_(o.owned_) {
        o.L_ = nullptr;
        o.owned_ = false;
    }
    state& operator=(state&& o) noexcept {
        if (this != &o) {
            close();
            L_ = o.L_;
            owned_ = o.owned_;
            o.L_ = nullptr;
            o.owned_ = false;
        }
        return *this;
    }
    ~state() { close(); }

    void open_libraries(int libs) { state_view::open_libraries(libs); }
    template <typename... Libs>
    void open_libraries(Libs... libs) {
        int mask = 0;
        ((mask |= static_cast<int>(libs)), ...);
        open_libraries(mask);
    }

private:
    void close() {
        if (owned_ && L_) lua_close(L_);
        owned_ = false;
        L_ = nullptr;
    }
    bool owned_ = false;
};

inline stack_object::stack_object(state_view sv, int idx)
    : L_(sv.lua_state()), idx_(lua_absindex(L_, idx)) {}

// ---- usertype
// ----------------------------------------------------------------

namespace detail {

template <typename T>
int usertype_gc_thunk(lua_State* L) {
    if (auto* p = static_cast<T*>(lua_touserdata(L, 1))) p->~T();
    return 0;
}

template <typename T>
int usertype_no_construct_thunk(lua_State* L) {
    luaL_error(L, "attempt to construct an unconstructable type");
    throw std::runtime_error("shd::lua: unreachable");  // luaL_error longjmps
}

// Argument unpacking for usertype callables: a `const T&`/`T&`/`T*` parameter
// reads the self userdata at the current stack slot; everything else goes
// through the regular conversion path.
template <typename T>
struct self_unpack {
    template <typename U>
    static U arg(lua_State* L, int& idx) {
        if constexpr (std::is_same_v<std::decay_t<U>, T> &&
                      (std::is_reference_v<U> || std::is_pointer_v<U>)) {
            using Ptr = std::add_pointer_t<std::remove_reference_t<U>>;
            const int i = idx++;
            auto* p = static_cast<Ptr>(lua_touserdata(L, i));
            if (p == nullptr) {
                luaL_error(L, "bad self argument #%d", i);
                throw std::runtime_error("shd::lua: unreachable");
            }
            if constexpr (std::is_pointer_v<U>) {
                return p;
            } else {
                return *p;
            }
        } else {
            return unpack_arg<U>(L, idx);
        }
    }
};

// Dispatcher for usertype callables (lambdas / free functions bound on the
// type): unpacks the full signature — including leading self-shaped params —
// from the raw stack arguments.
template <typename T, typename R, typename ArgsTuple, typename F>
struct self_dispatch_helper;  // primary
template <typename T, typename R, typename... A, typename F>
struct self_dispatch_helper<T, R, std::tuple<A...>, F> {
    static std::function<int(lua_State*)> make(F&& f) {
        return [f](lua_State* L) -> int {
            int idx = 1;
            // Pass the ORIGINAL parameter types (not decayed): self-shaped
            // detection needs the const T&/T* reference form intact.
            auto args = std::tuple<std::decay_t<A>...>{
                self_unpack<T>::template arg<A>(L, idx)...};
            if constexpr (std::is_void_v<R>) {
                std::apply(f, std::move(args));
                return 0;
            } else {
                R r = std::apply(f, std::move(args));
                return push_result(L, 1, std::move(r));
            }
        };
    }
};

template <typename F>
int usertype_callable_thunk(lua_State* L) {
    const std::function<int(lua_State*)>* disp =
        static_cast<const std::function<int(lua_State*)>*>(
            lua_touserdata(L, lua_upvalueindex(1)));
    // Catch handlers complete before luaL_error longjmps (see closure_thunk).
    char msgbuf[512] = {0};
    int kind = 0;
    try {
        return (*disp)(L);
    } catch (const std::exception& e) {
        std::snprintf(msgbuf, sizeof(msgbuf), "%s", e.what());
        kind = 1;
    } catch (...) {
        kind = 2;
    }
    if (kind == 2) return luaL_error(L, "unknown C++ exception");
    return luaL_error(L, "%s", msgbuf);
}

template <typename T, typename MemFn, typename R, typename... A>
int usertype_method_thunk(lua_State* L) {
    const MemFn* m =
        static_cast<const MemFn*>(lua_touserdata(L, lua_upvalueindex(1)));
    auto* self = static_cast<T*>(lua_touserdata(L, 1));
    if (self == nullptr || m == nullptr) {
        luaL_error(L, "method called without a self object");
        throw std::runtime_error("shd::lua: unreachable");
    }
    char msgbuf[512] = {0};
    int kind = 0;
    try {
        int idx = 2;
        auto args = std::tuple<std::decay_t<A>...>{
            unpack_arg<std::decay_t<A>>(L, idx)...};
        auto call = [&](auto&&... unpacked) -> R {
            return (self->**m)(std::forward<decltype(unpacked)>(unpacked)...);
        };
        if constexpr (std::is_void_v<R>) {
            std::apply(call, std::move(args));
            return 0;
        } else {
            R r = std::apply(call, std::move(args));
            return push_result(L, 1, std::move(r));
        }
    } catch (const std::exception& e) {
        std::snprintf(msgbuf, sizeof(msgbuf), "%s", e.what());
        kind = 1;
    } catch (...) {
        kind = 2;
    }
    if (kind == 2) return luaL_error(L, "unknown C++ exception");
    return luaL_error(L, "%s", msgbuf);
}

// Shared upvalue plumbing: stores a std::function dispatcher in a userdata
// upvalue guarded by a __gc metatable.
inline void store_dispatcher_upvalue(lua_State* L,
                                     std::function<int(lua_State*)>&& fn) {
    auto* store = static_cast<std::function<int(lua_State*)>*>(
        lua_newuserdatauv(L, sizeof(std::function<int(lua_State*)>), 1));
    new (store) std::function<int(lua_State*)>(std::move(fn));
    lua_newtable(L);
    lua_pushcfunction(L, [](lua_State* lg) -> int {
        auto* f =
            static_cast<std::function<int(lua_State*)>*>(lua_touserdata(lg, 1));
        f->~function();
        return 0;
    });
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
}

template <typename T, typename F>
void push_usertype_callable(lua_State* L, int target, const char* key, F&& f) {
    using traits = closure_traits<std::decay_t<F>>;
    store_dispatcher_upvalue(
        L, self_dispatch_helper<T, typename traits::ret, typename traits::args,
                                std::decay_t<F>>::make(std::forward<F>(f)));
    lua_pushcclosure(L, &usertype_callable_thunk<std::decay_t<F>>, 1);
    lua_setfield(L, target, key);
}

}  // namespace detail

// One "key", "value" binding of a usertype. Overload resolution picks the
// member-pointer forms, no_constructor, the metamethod keys, and the generic
// callable form.
template <typename T, typename R, typename... A>
void bind_usertype_entry(lua_State* L, int methods, int /*mt*/, const char* key,
                         R (T::*m)(A...)) {
    using MemFn = R (T::*)(A...);
    auto* store = static_cast<MemFn*>(lua_newuserdatauv(L, sizeof(MemFn), 0));
    *store = m;
    lua_pushcclosure(L, &detail::usertype_method_thunk<T, MemFn, R, A...>, 1);
    lua_setfield(L, methods, key);
}
template <typename T, typename R, typename... A>
void bind_usertype_entry(lua_State* L, int methods, int /*mt*/, const char* key,
                         R (T::*m)(A...) const) {
    using MemFn = R (T::*)(A...) const;
    auto* store = static_cast<MemFn*>(lua_newuserdatauv(L, sizeof(MemFn), 0));
    *store = m;
    lua_pushcclosure(L, &detail::usertype_method_thunk<T, MemFn, R, A...>, 1);
    lua_setfield(L, methods, key);
}
template <typename T>
void bind_usertype_entry(lua_State* L, int methods, int /*mt*/, const char* key,
                         no_constructor_t) {
    lua_pushcfunction(L, &detail::usertype_no_construct_thunk<T>);
    lua_setfield(L, methods, key);
}
template <typename T, typename F>
void bind_usertype_entry(lua_State* L, int /*methods*/, int mt,
                         meta_function::to_string_t, F&& f) {
    detail::push_usertype_callable<T>(L, mt, "__tostring", std::forward<F>(f));
}
template <typename T, typename F>
void bind_usertype_entry(lua_State* L, int /*methods*/, int mt,
                         meta_function::equal_to_t, F&& f) {
    detail::push_usertype_callable<T>(L, mt, "__eq", std::forward<F>(f));
}
template <typename T, typename F>
void bind_usertype_entry(lua_State* L, int methods, int /*mt*/, const char* key,
                         F&& f) {
    detail::push_usertype_callable<T>(L, methods, key, std::forward<F>(f));
}

template <typename T>
void bind_usertype_pairs(lua_State* /*L*/, int /*methods*/, int /*mt*/) {}
template <typename T, typename K, typename V, typename... Rest>
void bind_usertype_pairs(lua_State* L, int methods, int mt, K&& k, V&& v,
                         Rest&&... rest) {
    bind_usertype_entry<T>(L, methods, mt, std::forward<K>(k),
                           std::forward<V>(v));
    bind_usertype_pairs<T>(L, methods, mt, std::forward<Rest>(rest)...);
}

// Register a C++ type as a Lua userdata type (sol2 new_usertype parity for
// the subset Shield uses): methods go through __index, metamethods onto the
// metatable, "new" may be pinned to no_constructor, and the type name
// resolves to the methods table in the globals.
template <typename T, typename... Args>
void new_usertype(state_view sv, const std::string& name, Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0,
                  "new_usertype expects name/value pairs");
    shd::register_type_name<T>(name);
    lua_State* L = sv.lua_state();
    lua_newtable(L);
    const int methods = lua_gettop(L);
    lua_newtable(L);
    const int mt = lua_gettop(L);
    lua_pushvalue(L, methods);
    lua_setfield(L, mt, "__index");
    lua_pushcfunction(L, &detail::usertype_gc_thunk<T>);
    lua_setfield(L, mt, "__gc");
    // Register the metatable under the type name so push_userdata can
    // attach it to new instances.
    lua_pushvalue(L, mt);
    lua_setfield(L, LUA_REGISTRYINDEX, name.c_str());
    bind_usertype_pairs<T>(L, methods, mt, std::forward<Args>(args)...);
    lua_pushvalue(L, methods);
    lua_setglobal(L, name.c_str());
    lua_pop(L, 1);  // the metatable
}

// Create a userdata instance of a registered usertype (sol2
// make_object<T>(...) parity for usertyped values).
template <typename T, typename... A>
object make_userdata(state_view sv, const std::string& type_name, A&&... a) {
    lua_State* L = sv.lua_state();
    auto* p = static_cast<T*>(lua_newuserdatauv(L, sizeof(T), 1));
    new (p) T(std::forward<A>(a)...);
    luaL_getmetatable(L, type_name.c_str());
    lua_setmetatable(L, -2);
    lua_pushlstring(L, type_name.data(), type_name.size());
    lua_setiuservalue(L, -2, 1);
    object o(L, -1);
    lua_pop(L, 1);
    return o;
}

// ---- free helpers ----------------------------------------------------------

template <typename T>
object make_object(lua_State* L, T&& v) {
    detail::push(L, std::forward<T>(v));
    object o(L, -1);
    lua_pop(L, 1);
    return o;
}
inline object make_object(lua_State* L, nil_t) {
    lua_pushnil(L);
    object o(L, -1);
    lua_pop(L, 1);
    return o;
}
inline object make_object(lua_State* L, const char* v) {
    return make_object(L, std::string(v));
}
// state_view conveniences (sol::make_object(state_view, ...) parity).
template <typename T>
object make_object(state_view sv, T&& v) {
    return make_object(sv.lua_state(), std::forward<T>(v));
}
inline object make_object(state_view sv, nil_t) {
    return make_object(sv.lua_state(), nil);
}
inline object make_object(state_view sv, const char* v) {
    return make_object(sv.lua_state(), std::string(v));
}

}  // namespace shd
