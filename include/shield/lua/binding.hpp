// [SHIELD_LUA] Thin Lua C API binding layer (shd).
//
// Design notes (why this exists): a prior binding dependency was dropped;
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
// Error discipline is tuned for the Lua tests: argument
// and index conversion failures raise a Lua error inside the protected call
// (the script sees a runtime error; the host sees a !valid() result).
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

// The include order below IS the declaration order of the shd layers;
// each header only sees what the previous ones define.
// clang-format off
#include "shield/lua/binding/binding_core.hpp"
#include "shield/lua/binding/binding_stack_object.hpp"
#include "shield/lua/binding/binding_table_object.hpp"
#include "shield/lua/binding/binding_closures.hpp"
#include "shield/lua/binding/binding_state.hpp"
#include "shield/lua/binding/binding_state_view.hpp"
#include "shield/lua/binding/binding_usertype.hpp"
// clang-format on
