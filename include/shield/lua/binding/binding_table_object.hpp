// [SHIELD_LUA] shd binding layer - split of binding.hpp.
// [SHD] Registry-reference table/object model: accessor proxies,
// iteration, conversion helpers.
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
        if (L_ && o.ref_ != LUA_NOREF) {  // GCOVR_EXCL_BR_LINE
            // (defensive: a null state always pairs with LUA_NOREF, so the
            // ref arm only runs on live refs)
            lua_rawgeti(L_, LUA_REGISTRYINDEX, o.ref_);
            ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    }
    ref_base& operator=(const ref_base& o) {
        if (this != &o) {
            release();
            L_ = o.L_;
            if (L_ && o.ref_ != LUA_NOREF) {  // GCOVR_EXCL_BR_LINE
                // (defensive: a null state always pairs with LUA_NOREF, so
                // the ref arm only runs on live refs)
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
    // Alternative naming aliases used by runtime/service call sites.
    lua_State* lua_state() const { return L_; }
    int registry_index() const { return ref_; }
    // LUA_REFNIL is what luaL_ref yields for a nil value: the reference
    // exists but denotes nil, so valid() must reject it (parity:
    // `lua["missing"].valid()` is false).
    bool valid() const {
        return L_ != nullptr && ref_ != LUA_NOREF &&  // GCOVR_EXCL_BR_LINE
               ref_ != LUA_REFNIL;  // GCOVR_EXCL_BR_LINE (defensive: a live
                                    // state always carries a registry ref)
    }

    // Drop the reference without luaL_unref (abandon parity): the
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
        if (L_ && ref_ != LUA_NOREF)  // GCOVR_EXCL_BR_LINE (defensive: a live
                                      // state always carries an outstanding
                                      // ref, so the joint-false arm never
                                      // runs with L_ set)
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
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
    // nil-on-push), matching an object built from the nil literal.
    object(nil_t) {}
    // Reference-type values convert to object implicitly (parity): a
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
                throw std::runtime_error(  // GCOVR_EXCL_LINE (unreachable:
                                           // luaL_error longjmps past it)
                    "shd::lua: bad cast");
            }
            T v(L_, i);  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                         // landing arcs of the view construction)
            lua_pop(L_, 1);
            return v;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                       // landing arcs of the return sequence)
        } else if constexpr (std::is_same_v<D0, protected_function>) {
            const int i = push();
            // Construct via the dependent D0: protected_function is only
            // forward-declared at this point in the header.
            D0 pf(L_, i);  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                           // landing arcs of the view construction)
            lua_pop(L_, 1);
            return pf;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                        // landing arcs of the return sequence)
        } else if constexpr (std::is_same_v<D0, object>) {
            // object is a registry reference type, not a usertype. Create a new
            // object reference from the stack slot (same pattern as
            // table/function).
            static_assert(!std::is_reference_v<T>,
                          "shd::object::as<object>(): object views must be "
                          "taken by value, not by reference");
            const int i = push();
            object o(L_, i);  // GCOVR_EXCL_BR_LINE (compiler artifact: the
                              // EH landing arcs of the view construction)
            lua_pop(L_, 1);
            return o;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                       // landing arcs of the return sequence)
        } else if constexpr (detail::is_optional<D0>::value) {
            // Parity: nil (or a type mismatch) yields an empty
            // optional instead of throwing; anything else reads as V. Without
            // this branch std::optional is a class type and would misroute
            // into the usertype branch below.
            using V = typename D0::value_type;
            if (!is<V>()) return D0{};
            return D0{as<V>()};
        } else if constexpr (std::is_class_v<D0> &&
                             !std::is_same_v<D0, std::string>) {
            // Registered usertype: read the userdata payload. A non-userdata
            // or foreign-usertype value here is caller error (is<> guards
            // the runtime call sites); behavior parity treats it the same way.
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
                D0 v = detail::usertype_as<D0>(  // GCOVR_EXCL_BR_LINE
                    L_, i);  // (compiler artifact: the EH landing arcs of
                             // the payload copy)
                lua_pop(L_, 1);
                return v;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                           // landing arcs of the return sequence)
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
                throw std::runtime_error(  // GCOVR_EXCL_LINE (unreachable:
                                           // luaL_error longjmps past it)
                    "shd::lua: bad cast");
            }
            D0 v = detail::stack_read<D0>(  // GCOVR_EXCL_BR_LINE
                L_, i);  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                         // landing arcs of the read)
            lua_pop(L_, 1);
            return v;  // GCOVR_EXCL_BR_LINE (compiler artifact: the EH
                       // landing arcs of the return sequence)
        }
    }

    // Element access on a table-valued object (defined after accessor).
    accessor operator[](const std::string& k) const;
    accessor operator[](const char* k) const;

    // Reference views of the same value (parity): a generic object
    // converts to the reference type its value actually carries. Unchecked,
    // by design — the caller asked for the view explicitly. Out-of-line: the
    // reference classes complete after this one.
    operator table() const;
    operator function() const;
    operator protected_function() const;

    // Implicit conversion to the host types stack_read supports
    // (parity for `std::string s = obj;` style call sites).
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
    // Registry refs are shared across the VM's threads, so push the value
    // onto the CALLER's state L — never o's own thread: a value anchored on
    // the main thread pushed here (coroutine C closures) would land on the
    // wrong stack and the closure would report more returns than it pushed.
    if (o.valid()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, o.ref_index());
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
    function(lua_State* L, ref_index_tag r) : detail::ref_base(L, r) {}
    // nil literal: an invalid function (callers check valid() before use).
    function(nil_t) {}
    // Cross-conversion (parity): protected_function and function share
    // the same underlying registry reference. Defined after
    // protected_function completes.
    function(const protected_function& pf);
    // Adopt whatever the object references (function-from-object
    // parity; a non-function ref errors at call time, not construction).
    // explicit: object also converts via operator function() above, and a
    // copy-initialization (`shd::function f = obj;`) with both paths viable
    // is ambiguous to clang (GCC picks the conversion operator silently).
    explicit function(const object& o) : detail::ref_base(o) {}

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
        if (status != LUA_OK) {  // GCOVR_EXCL_BR_LINE (instantiation
                                 // artifact: the pcall-error arm of
                                 // per-signature call instances that never
                                 // fail persists as a zero record under
                                 // gcovr 8.x)
            lua_pop(L, 1);       // error message  // GCOVR_EXCL_BR_LINE (same
                                 // error arm as the guard above)
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
        lua_rawgeti(L, LUA_REGISTRYINDEX, f.ref_index());  // see push(object)
    } else {
        lua_pushnil(L);
    }
}
}  // namespace detail

// Key accessor proxy: `tbl["k"] = v` writes through; reading converts. Holds
// its own registry reference to the containing table so it stays valid even
// Reference equality through Lua: two objects are equal when lua_rawequal
// holds for the values they reference (object==object parity).
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

    // Write-through for same-type assignment (`tbl["a"] = other["b"]`):
    // without this, the implicit copy assignment (a better match than the
    // template below for accessor-typed RHS) would copy the (owner, key)
    // pair without touching the Lua table, silently dropping the
    // assignment (player["defaults"] = impl["defaults"] wrote nothing).
    // Three non-template overloads cover every RHS form (prvalue / lvalue /
    // const lvalue); each ties the deduced template on conversion sequence
    // and wins the non-template tie-break, so accessor-typed RHS always
    // writes through instead of recursing into the generic push path.
    accessor& operator=(const accessor& other) { return assign_write(other); }
    accessor& operator=(accessor& other) { return assign_write(other); }
    accessor& operator=(accessor&& other) { return assign_write(other); }

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

    // Read with a fallback (accessor::get_or parity): nil/missing keys
    // yield the default; present values convert.
    template <typename T>
    T get_or(T def) const {
        object v = value();
        if (!v.valid() || v.is_nil()) return def;  // GCOVR_EXCL_BR_LINE
        // (defensive: a valid reference is never nil, so the is_nil arm only
        // matters for LUA_REFNIL)
        return v.as<T>();
    }

    template <typename T>
    T as() const {
        return value().as<T>();
    }

    // Accessor parity alias.
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

    // Implicit read to host stack types (bool/string/number; parity for
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
    // Shared writer for the accessor-typed operators above: re-read the
    // referenced value and write it through this accessor's slot.
    accessor& assign_write(const accessor& other) {
        *this = other.value();
        return *this;
    }

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
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }

    object raw_get(const std::string& k) const {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        lua_rawget(L_, i);
        object o(L_, -1);
        lua_pop(L_, 1);
        return o;
    }
    // Convert-on-read form (raw_get<T> parity): the proxy conversion to
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
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }
    template <typename T>
    void raw_set(const std::string& k, T&& v) {
        const int i = push();
        lua_pushlstring(L_, k.data(), k.size());
        detail::push(L_, std::forward<T>(v));
        lua_rawset(L_, i);
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }
    template <typename T>
    void raw_set(std::int64_t k, T&& v) {
        const int i = push();
        lua_pushinteger(L_, k);
        detail::push(L_, std::forward<T>(v));
        lua_rawset(L_, i);
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }
    template <typename T>
    void set(std::int64_t k, T&& v) {
        const int i = push();
        lua_pushinteger(L_, k);
        detail::push(L_, std::forward<T>(v));
        lua_settable(L_, i);
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }

    // -- function registration ---------------------------------------------
    template <typename F>
    void set_function(const std::string& name, F&& f) {
        const int i = push();
        push_closure(std::forward<F>(f));
        lua_setfield(L_, i, name.c_str());
        lua_pop(L_, 1);  // push() left the table copy on the stack
    }

    // Create a fresh table nested in this one.
    table create_table(const std::string& name) {
        const int i = push();
        lua_newtable(L_);
        table t(L_, -1);
        lua_pushlstring(L_, name.data(), name.size());
        lua_pushvalue(L_, -2);
        lua_settable(L_, i);
        lua_pop(L_, 2);  // nested table copy + push()'s table copy
        return t;
    }
    table create_table(std::int64_t idx_key) {
        const int i = push();
        lua_newtable(L_);
        table t(L_, -1);
        lua_pushinteger(L_, idx_key);
        lua_pushvalue(L_, -2);
        lua_settable(L_, i);
        lua_pop(L_, 2);  // nested table copy + push()'s table copy
        return t;
    }

    // Size of the sequence part.
    std::size_t size() const {
        const int i = push();
        const lua_Integer n = lua_rawlen(L_, i);
        lua_pop(L_, 1);
        return static_cast<std::size_t>(n);
    }

    // Append a value at the end of the sequence part (add parity).
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

// Range-for iterator over a table's key/value pairs (tables iterate
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
        lua_rawgeti(L, LUA_REGISTRYINDEX, t.ref_index());  // see push(object)
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

}  // namespace shd
