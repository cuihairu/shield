// [SHIELD_LUA] shd binding layer - split of binding.hpp.
// [SHD] Forward declarations, stack_object non-owning views and the
// usertype name registry.
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

// ---- forward decls --------------------------------------------------------

class object;
class accessor;

// ---- stack_object (full definition) ----------------------------------------

// Non-owning view of a value at a fixed stack position.
// The index is absolutized on construction; the
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
        } else if constexpr (std::is_class_v<D> &&
                             !std::is_same_v<D, std::string> &&
                             !std::is_same_v<D, const char*>) {
            return detail::usertype_is<D>(L_, idx_);
        } else {
            return detail::stack_check<D>(L_, idx_);
        }
    }

    template <typename T>
    T as() const {
        using D = std::remove_const_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<D, table>) {
            // Construct via the dependent D (see unpack_arg): the class is
            // incomplete at this point in the header for some targets.
            return D(L_, idx_);
        } else if constexpr (std::is_same_v<D, function>) {
            return D(L_, idx_);
        } else if constexpr (std::is_same_v<D, object>) {
            return D(L_, idx_);
        } else if constexpr (std::is_class_v<D> &&
                             !std::is_same_v<D, std::string> &&
                             !std::is_same_v<D, const char*>) {
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
    // No operator object(): object(const stack_object&) below is the single
    // conversion path. Both a converting constructor and a conversion
    // operator would make stack_object -> object ambiguous on clang/MSVC.

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
// different binding surface (the legacy surface registers ClientContext and
// friends) records itself via register_type_name<T> plus a registry mirror of
// its metatable under that name. The per-state lookup keeps a global name
// list correct across many VMs.

// Global type-name registry with external linkage (avoids per-TU statics in
// the function template). Key is std::type_index, value is vector of names.
inline std::unordered_map<std::type_index, std::vector<std::string>>&
type_name_registry() {
    static std::unordered_map<
        std::type_index,  // GCOVR_EXCL_BR_LINE
                          // (compiler artifact: the thread-safe static
                          // local's first-use guard pseudo-branch)
        std::vector<std::string>>
        reg;  // GCOVR_EXCL_BR_LINE (compiler artifact: the static guard's
              // continuation arcs land on the declarator)
    return reg;
}

// Serializes all registry access: service VM setup (and therefore
// register_type_name) runs concurrently on spawn workers and caller threads,
// so registration races registration and registration races lookups on
// actor threads. An unlocked vector here corrupted heap state that only
// surfaced when the process tore down.
inline std::mutex& type_name_mutex() {
    static std::mutex m;  // GCOVR_EXCL_BR_LINE (compiler artifact: the
                          // thread-safe static local's first-use guard
                          // pseudo-branch)
    return m;
}

template <typename T>
std::vector<std::string>& type_names() {
    return type_name_registry()[std::type_index(typeid(T))];
}

// Read side: snapshot under the registry lock. Usertype checks run on actor
// threads while another service VM may be mid-setup, so returning a
// reference would race push_back's reallocation.
template <typename T>
std::vector<std::string> type_names_snapshot() {
    std::lock_guard lock(type_name_mutex());
    return type_names<T>();
}
namespace detail {

template <typename D>
bool usertype_is(lua_State* L, const int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA || lua_getmetatable(L, idx) == 0) {
        return false;
    }
    const std::vector<std::string> names = type_names_snapshot<D>();
    bool ok = false;
    for (const auto& name : names) {
        luaL_getmetatable(L, name.c_str());
        ok = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 1);
        if (ok) break;
    }
    lua_pop(L, 1);  // the value's metatable
    if (!ok) return false;
    // shd-created payloads carry an authoritative type tag in uservalue 1
    // (B1 dual-world seam): a foreign binding shares one generic metatable
    // across unregistered boxes, so when a tag is present it decides.
    if (lua_getiuservalue(L, idx, 1) == LUA_TSTRING) {  // GCOVR_EXCL_BR_LINE
        // (instantiation artifact: the coverate-only CovBox instance never
        // reaches the tag check, so its two jump records stay zero under
        // gcovr 8.x; the tag-present arm drives the real boxes)
        const char* tag = lua_tostring(L, -1);
        ok = false;
        for (const auto& name : names) {
            if (tag && name == tag) {  // GCOVR_EXCL_BR_LINE (defensive: the
                // uservalue was confirmed a string on the guard above, so
                // lua_tostring never returns null here)
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
    std::lock_guard lock(type_name_mutex());
    auto& names = type_names<T>();
    // Dedup: every service VM setup re-registers the same names; without
    // this the per-check lookup lists grow with the number of spawned
    // services.
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

namespace detail {

// push() for a host type marked as a usertype value: create the userdata and
// attach the metatable registered by new_usertype<T> (first registered name).
// Defined here rather than with the other push overloads because it needs the
// type-name registry.
template <typename T, typename Unused>
void push(lua_State* L, const T& v) {
    const std::vector<std::string> names = type_names_snapshot<T>();
    if (names.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: is_usertype_value
                          // specializations live next to their new_usertype
                          // registrations, so a pushed value always has names)
        // GCOVR_EXCL_START (defensive: the error arm is unreachable - see
        // the registration guard on the if above)
        luaL_error(L, "shd: usertype %s is not registered", typeid(T).name());
        throw std::runtime_error(
            "shd: unregistered usertype");  // luaL_error longjmps past it
        // GCOVR_EXCL_STOP
    }
    if constexpr (is_foreign_usertype<T>::value) {
        // Foreign usertype (registered outside shd): use pointer-box layout to
        // match the foreign metatable's __gc expectation. Allocate on heap,
        // store pointer in userdata, attach the foreign metatable.
        auto* heap = new T(v);
        auto** p = static_cast<T**>(lua_newuserdatauv(L, sizeof(T*), 0));
        *p = heap;
        luaL_getmetatable(L, names.front().c_str());
        lua_setmetatable(L, -2);
        // No uservalue tag: is_shd_raw_userdata will return false, so the
        // foreign path in box_context_marker will be taken (which uses
        // a plain object to read the pointer-box layout).
    } else {
        // Native shd usertype: value storage with type-name tag in uservalue 1.
        auto* p = static_cast<T*>(lua_newuserdatauv(L, sizeof(T), 1));
        new (p) T(v);  // GCOVR_EXCL_BR_LINE (compiler artifact: placement
                       // construction's arcs of T's constructor)
        luaL_getmetatable(L, names.front().c_str());
        lua_setmetatable(L, -2);
        lua_pushlstring(L, names.front().data(), names.front().size());
        lua_setiuservalue(L, -2, 1);
    }
}

// Rvalue overload for usertype values returned by value from bound methods.
// Moves the value into the userdata storage.
template <typename T, typename Unused>
void push(lua_State* L, T&& v) {
    push(L, static_cast<const T&>(v));
}

// Layout tag for the B1 dual-world seam: shd-created usertype userdata
// carries its type name in uservalue 1 with the payload raw (T in place),
// while foreign-created boxes store the payload in a binding-internal box
// layout with no uservalues. The mirrored metatables give identity only (the
// legacy binding shares one generic metatable for unregistered boxes), so the
// tag distinguishes both layout and exact type.
inline bool is_shd_raw_userdata(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA) return false;
    const int t = lua_getiuservalue(L, idx, 1);
    lua_pop(L, 1);
    return t == LUA_TSTRING;
}

}  // namespace detail

}  // namespace shd
