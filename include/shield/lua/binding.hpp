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
//   * this_state carrier for lambdas that need the raw lua_State*.
// Deliberately NOT implemented (no call site needs it): usertypes with
// metatables, coroutine ownership, thread pools.
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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace shd {

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

// ---- stack push/traits ----------------------------------------------------

// Referencing classes defined further down; declared here at shd scope so
// the detail:: declarations below (and dependent lookups in templates) bind
// to the real types.
class table;
class object;
class function;
class accessor;
class variadic_results;

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

void push(lua_State* L, const table& t);
void push(lua_State* L, const object& o);
void push(lua_State* L, const function& f);

template <typename T>
void push(lua_State* L, const std::optional<T>& v) {
    if (v)
        push(L, *v);
    else
        push(L, nil);
}

// Type-tagged stack check: can the value at idx be read as T?
template <typename T>
struct is_stack : std::false_type {};
template <>
struct is_stack<bool> : std::true_type {};
template <>
struct is_stack<std::int64_t> : std::true_type {};
template <>
struct is_stack<int> : std::true_type {};
template <>
struct is_stack<std::uint64_t> : std::true_type {};
template <>
struct is_stack<double> : std::true_type {};
template <>
struct is_stack<float> : std::true_type {};
template <>
struct is_stack<std::string> : std::true_type {};
template <>
struct is_stack<const char*> : std::true_type {};

template <typename T>
bool stack_check(lua_State* L, int idx);

template <>
inline bool stack_check<bool>(lua_State* L, int idx) {
    return lua_isboolean(L, idx);
}
// Integer binds accept only true Lua integers (sol2 parity: a float argument
// is a type error, not a silent truncation).
template <>
inline bool stack_check<std::int64_t>(lua_State* L, int idx) {
    return lua_isinteger(L, idx) != 0;
}
template <>
inline bool stack_check<int>(lua_State* L, int idx) {
    return lua_isinteger(L, idx) != 0;
}
template <>
inline bool stack_check<std::uint64_t>(lua_State* L, int idx) {
    return lua_isinteger(L, idx) != 0;
}
template <>
inline bool stack_check<double>(lua_State* L, int idx) {
    return lua_isnumber(L, idx) != 0;
}
template <>
inline bool stack_check<float>(lua_State* L, int idx) {
    return lua_isnumber(L, idx) != 0;
}
// String binds accept only actual strings (numbers do not silently coerce).
template <>
inline bool stack_check<std::string>(lua_State* L, int idx) {
    return lua_type(L, idx) == LUA_TSTRING;
}
template <>
inline bool stack_check<const char*>(lua_State* L, int idx) {
    return lua_type(L, idx) == LUA_TSTRING;
}

// Read a value of type T off the stack.
template <typename T>
T stack_read(lua_State* L, int idx);

template <>
inline bool stack_read<bool>(lua_State* L, int idx) {
    return lua_toboolean(L, idx) != 0;
}
template <>
inline std::int64_t stack_read<std::int64_t>(lua_State* L, int idx) {
    return static_cast<std::int64_t>(lua_tointeger(L, idx));
}
template <>
inline int stack_read<int>(lua_State* L, int idx) {
    return static_cast<int>(lua_tointeger(L, idx));
}
template <>
inline std::uint64_t stack_read<std::uint64_t>(lua_State* L, int idx) {
    return static_cast<std::uint64_t>(lua_tointeger(L, idx));
}
template <>
inline double stack_read<double>(lua_State* L, int idx) {
    return lua_tonumber(L, idx);
}
template <>
inline float stack_read<float>(lua_State* L, int idx) {
    return static_cast<float>(lua_tonumber(L, idx));
}
template <>
inline std::string stack_read<std::string>(lua_State* L, int idx) {
    size_t len = 0;
    const char* s = lua_tolstring(L, idx, &len);
    return std::string(s ? s : "", s ? len : 0);
}
template <>
inline const char* stack_read<const char*>(lua_State* L, int idx) {
    return lua_tostring(L, idx);
}

// Lua type name for a host type (for error messages).
template <typename T>
const char* host_type_name() {
    return "value";
}
template <>
inline const char* host_type_name<bool>() {
    return "boolean";
}
template <>
inline const char* host_type_name<std::int64_t>() {
    return "integer";
}
template <>
inline const char* host_type_name<int>() {
    return "integer";
}
template <>
inline const char* host_type_name<std::uint64_t>() {
    return "integer";
}
template <>
inline const char* host_type_name<double>() {
    return "number";
}
template <>
inline const char* host_type_name<float>() {
    return "number";
}
template <>
inline const char* host_type_name<std::string>() {
    return "string";
}
template <>
inline const char* host_type_name<const char*>() {
    return "string";
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
    if constexpr (std::is_same_v<T, this_state>) {
        (void)idx;
        return this_state(L);
    } else if constexpr (std::is_same_v<T, lua_State*>) {
        (void)idx;
        return L;
    } else {
        const int i = idx++;
        if (!stack_check<T>(L, i)) {
            luaL_error(L, "bad argument #%d (%s expected, got %s)", i,
                       host_type_name<T>(), luaL_typename(L, i));
            // luaL_error longjmps into lua_pcall; the throw below is
            // unreachable and only satisfies the return type.
            throw std::runtime_error("shd::lua: argument conversion failed");
        }
        return stack_read<T>(L, i);
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

}  // namespace detail

// ---- forward decls --------------------------------------------------------

class object;
class accessor;

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
    bool valid() const { return L_ != nullptr && ref_ != LUA_NOREF; }

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

    template <typename T>
    bool is() const {
        if (!valid()) return std::is_same_v<T, nil_t>;
        const int i = push();
        const bool ok = detail::stack_check<T>(L_, i);
        lua_pop(L_, 1);
        return ok;
    }

    template <typename T>
    T as() const {
        const int i = push();
        if (!detail::stack_check<T>(L_, i)) {
            luaL_error(L_, "bad cast (%s expected, got %s)",
                       detail::host_type_name<T>(), luaL_typename(L_, i));
            // luaL_error longjmps into the enclosing pcall; unreachable.
            throw std::runtime_error("shd::lua: bad cast");
        }
        T v = detail::stack_read<T>(L_, i);
        lua_pop(L_, 1);
        return v;
    }

    bool operator==(const nil_t&) const { return is_nil(); }
    bool operator!=(const nil_t&) const { return !is_nil(); }

    // Element access on a table-valued object (defined after accessor).
    accessor operator[](const std::string& k) const;
    accessor operator[](const char* k) const;

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

// Referenced Lua function (callable via protected call).
class function : public detail::ref_base {
public:
    function() = default;
    function(lua_State* L, int idx) : detail::ref_base(L, idx) {}

    // Protected call with variadic args; returns the first result or nil.
    template <typename... Args>
    object call(Args&&... args) const {
        lua_State* L = state();
        push();  // the callable
        (detail::push(L, std::forward<Args>(args)), ...);
        const int nargs = static_cast<int>(sizeof...(Args));
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

    template <typename T>
    T as() const {
        return value().as<T>();
    }

    template <typename T>
    bool is() const {
        return value().is<T>();
    }

    operator object() const { return value(); }
    operator function() const {
        object v = value();
        if (!v.valid()) return function();
        return function(v.state(), v.push());
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

    object raw_get(const std::string& k) const {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        lua_rawget(L_, i);
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
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

private:
    // Closure trampoline: upvalue 1 carries a std::function dispatcher.
    template <typename F>
    void push_closure(F&& f);
};

namespace detail {
inline void push(lua_State* L, const table& t) {
    if (t.valid()) {
        t.push();
    } else {
        lua_pushnil(L);
    }
}
}  // namespace detail

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
    const std::string& what() const noexcept { return msg_; }
    explicit operator bool() const { return !msg_.empty(); }

private:
    std::string msg_;
};

class protected_function_result {
public:
    protected_function_result() = default;
    protected_function_result(lua_State* L, int status, object first)
        : status_(status), first_(std::move(first)) {
        (void)L;
    }

    bool valid() const { return status_ == LUA_OK; }
    int status() const { return status_; }
    error get_error() const {
        if (valid()) return error();
        return error(first_.valid() ? first_.as<std::string>()
                                    : std::string("unknown error"));
    }
    // First returned value (the error message when the call failed).
    const object& get() const { return first_; }
    template <typename T>
    T get() const {
        return first_.as<T>();
    }

private:
    int status_ = LUA_ERRRUN;
    object first_;
};

class protected_function : public detail::ref_base {
public:
    protected_function() = default;
    explicit protected_function(const object& o) {
        if (o.valid()) {
            L_ = o.state();
            o.push();
            ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    }
    protected_function(lua_State* L, int idx) : detail::ref_base(L, idx) {}

    template <typename... Args>
    protected_function_result call(Args&&... args) const {
        lua_State* L = state();
        if (!valid()) {
            return protected_function_result(L, LUA_ERRRUN, object());
        }
        push();  // the callable
        (detail::push(L, std::forward<Args>(args)), ...);
        const int nargs = static_cast<int>(sizeof...(Args));
        const int status = lua_pcall(L, nargs, 1, 0);
        object first(L, -1);
        lua_pop(L, 1);
        return protected_function_result(L, status, std::move(first));
    }

    template <typename... Args>
    protected_function_result operator()(Args&&... args) const {
        return call(std::forward<Args>(args)...);
    }
};

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

    protected_function_result load(
        const std::string& code,
        const std::string& chunkname = "string") const {
        return load_protected(chunkname, code);
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
        if (libs & lib::table) {
            luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1);
            lua_pop(L_, 1);
        }
        if (libs & lib::debug) {
            luaL_requiref(L_, LUA_DBLIBNAME, luaopen_debug, 1);
            lua_pop(L_, 1);
        }
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
            object err(L_, -1);
            lua_pop(L_, 1);
            return protected_function_result(L_, status, std::move(err));
        }
        const int run = lua_pcall(L_, 0, 1, 0);
        object first(L_, -1);
        lua_pop(L_, 1);
        return protected_function_result(L_, run, std::move(first));
    }
};

class state : public state_view {
public:
    state() : state_view(luaL_newstate()), owned_(true) {
        if (!L_)
            throw std::runtime_error("shd::state: lua state allocation failed");
    }
    explicit state(lua_State* existing) : state_view(existing), owned_(false) {}
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

private:
    void close() {
        if (owned_ && L_) lua_close(L_);
        owned_ = false;
        L_ = nullptr;
    }
    bool owned_ = false;
};

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

}  // namespace shd
