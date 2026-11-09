// [SHIELD_LUA] Lua Runtime implementation (core): VM lifecycle,
// sandboxing, script cache, API registration and global data.
// Calls/http/pack live in the sibling translation units.
#include "shield/lua/lua_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lua_panic.hpp"
#include "lua_runtime_impl.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

// Diagnostic panic handler. The default sol at_panic throws with only the
// generic "An unexpected error occurred" message, which hides the real Lua
// error (and whether it was a non-string error object) from host logs.
//
// Semantics: forensics, then abort. Every production call path into Lua is
// protected (pcall / lua_resume / explicitly protected_function), so an
// error reaching at_panic is by construction a C++-side misuse of an
// unprotected call or a kernel-level failure — there is no recovering the
// VM. The handler therefore must not throw: a C++ exception cannot
// reliably unwind through the C Lua frames (luaD_throw is longjmp in a C
// build; MSVC loses it — see the c8352e9 Windows CI data point), and the
// Lua 5.5 kernel abort()s right after a returning handler anyway (ldo.c
// luaD_throw). Aborting explicitly keeps the message ours and the
// semantics identical on every platform.
//
// Note on state: Lua 5.5 resets the panicking thread before the handler
// runs (luaD_throw -> luaE_resetthread). The error object survives at the
// bottom of the reset stack (reachable at index -1, top == 1) but the
// frame chain is gone, so the traceback prints no Lua frames on 5.5; it is
// kept because it is free and future-proof.
std::string write_lua_panic_forensics(lua_State* L, std::FILE* sink) {
    const int entry_top = lua_gettop(L);
    std::string detail;
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* msg = lua_tostring(L, -1);
        detail = msg != nullptr
                     ? msg
                     : "<null error string>";  // GCOVR_EXCL_BR_LINE (defensive:
                                               // the LUA_TSTRING guard above
                                               // makes lua_tostring non-null)
    } else {
        detail = std::string("non-string error object (type=") +
                 lua_typename(L, lua_type(L, -1)) + ")";
    }
    // Forensics for CI-only panics: which thread held which VM and what was
    // on its stack when the unprotected error crossed the C boundary.
    {
        std::string dump =
            std::string(shd::main_thread(L) == L ? "main" : "coroutine") +
            " depth=" + std::to_string(lua_gettop(L)) + " [";
        for (int i = 1; i <= lua_gettop(L) && i <= 16; ++i) {
            if (i > 1) dump += ", ";
            dump +=
                "#" + std::to_string(i) + ":" + lua_typename(L, lua_type(L, i));
        }
        dump += "]";
        std::fprintf(sink, "*** shield lua panic ctx: tid=%zu state=%s\n",
                     static_cast<size_t>(std::hash<std::thread::id>{}(
                         std::this_thread::get_id())),
                     dump.c_str());
        std::fflush(sink);
    }
    luaL_traceback(L, L, detail.c_str(), 0);
    const char* tb = lua_tostring(L, -1);
    // Ternary into a local: inline in the fprintf call it is a gcov
    // aggregation artifact (the continuation line never gets credited).
    const char* tb_msg =
        tb != nullptr
            ? tb  // GCOVR_EXCL_BR_LINE (defensive: luaL_traceback always pushes
                  // a string)
            : detail.c_str();  // GCOVR_EXCL_BR_LINE (defensive: luaL_traceback
                               // always pushes a string)
    std::fprintf(sink, "*** shield lua panic: %s\n", tb_msg);
    std::fflush(sink);
    lua_settop(L, entry_top);  // tests drive this on live states
    return detail;
}  // GCOVR_EXCL_LINE (function-exit arc artifact: the aborting caller in

namespace {
// shield_lua_panic never returns, so the exit arc starves; the body above
// is covered in-process by the forensics tests)

// GCOVR_EXCL_START (abort path: this body only ever executes inside the
// death-test child, which dies by SIGABRT before gcov flushes its
// counters; the formatting itself is covered in-process through
// write_lua_panic_forensics)
int shield_lua_panic(lua_State* L) {
    write_lua_panic_forensics(
        L, stderr);  // GCOVR_EXCL_BR_LINE (defensive: only reached in the fork
                     // death-test child, whose gcov counters never flush)
    std::abort();
}
// GCOVR_EXCL_STOP
}  // namespace

void install_panic_handler(const std::shared_ptr<shd::state>& state) {
    lua_atpanic(state->lua_state(), shield_lua_panic);
}

// ============================================================================
// ServiceHandle Implementation
// ============================================================================

void ServiceHandle::register_usertype(shd::state_view lua) {
    lua.new_usertype<ServiceHandle>(
        "ServiceHandle",
        // Prevent direct construction from Lua
        "new", shd::no_constructor,

        // Methods
        // GCOVR_EXCL_START (wrappers are deduplicated by the linker into
        // the calling TU; behavior tested by ServiceHandleMetamethods)
        "id", &ServiceHandle::id, "node", &ServiceHandle::node, "valid",
        &ServiceHandle::valid,
        // GCOVR_EXCL_STOP

        // GCOVR_EXCL_START (linker dedup artifact)
        // Metamethods
        shd::meta_function::to_string,
        [](const ServiceHandle& h) {
            // GCOVR_EXCL_STOP
            return "<ServiceHandle: " + h.id() +
                   ">";  // GCOVR_EXCL_LINE (linker dedup artifact)
        },

        // GCOVR_EXCL_START (linker dedup artifact)
        // Equality by service ID
        shd::meta_function::equal_to,
        [](const ServiceHandle& a, const ServiceHandle& b) {
            // GCOVR_EXCL_STOP
            return a.id() == b.id();  // GCOVR_EXCL_LINE (linker dedup artifact)
        });
}

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}

LuaRuntime::~LuaRuntime() = default;

std::shared_ptr<LuaVM> LuaRuntime::create_vm() {
    auto vm = std::make_shared<LuaVM>();
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->vms_by_state[vm->state()->lua_state()] = vm;
    return vm;
}

shd::state& LuaRuntime::vm_state(const std::shared_ptr<LuaVM>& vm) {
    return *vm->state();
}

// GCOVR_EXCL_START (server-only helper: the CI coverage shape builds with
// SHIELD_ENABLE_SERVER=OFF, so this has no caller there; the full-build tree
// exercises it through the shield.server.watch facade)
shd::table LuaRuntime::service_table(const std::shared_ptr<LuaVM>& vm) const {
    if (!vm) {
        return shd::nil;
    }
    return vm->service_table();
}

// GCOVR_EXCL_STOP

void LuaRuntime::restrict_vm(std::shared_ptr<LuaVM> vm) {
    if (!vm) return;
    shd::state& state = *vm->state();

    // os: keep time/date/clock (business-clock hooks live there); drop the
    // process- and host-control functions. Guarded read: os is genuinely
    // absent when the VM was created with lua.sandbox.allow_os =false, so
    // the accessor is validated before the table view is taken.
    shd::accessor os_acc = state["os"];
    if (os_acc.valid() && os_acc.is<shd::table>()) {  // GCOVR_EXCL_BR_LINE
        // (defensive: os is a table whenever the accessor resolves; the
        // sandbox removes it entirely instead)
        shd::table os_table = os_acc.get<shd::table>();
        os_table["execute"] = shd::nil;
        os_table["exit"] = shd::nil;
        os_table["getenv"] = shd::nil;
        os_table["remove"] = shd::nil;
        os_table["rename"] = shd::nil;
        os_table["setlocale"] = shd::nil;
    }
    // io: eval diagnostics need no file or process IO.
    state["io"] = shd::nil;
    // require/package: snippets cannot pull modules off the local disk.
    state["require"] = shd::nil;
    state["package"] = shd::nil;
}

bool LuaRuntime::load_script(std::shared_ptr<LuaVM> vm,
                             std::string_view script_path) {
    // Protected load + exec (luaL_loadfile / lua_pcall): syntax, IO and
    // runtime errors come back as a status plus an error object on the
    // stack instead of being raised. sol's script_file is only protected in
    // Debug (SOL_SAFE); under NDEBUG it degrades to luaL_dofile, whose
    // failure surfaces through at_panic — now an abort — so the failure
    // must never be raised in the first place (c2ce720 Release CI).
    lua_State* L = vm->state()->lua_state();
    if (luaL_loadfile(L, std::string(script_path).c_str()) != LUA_OK) {
        lua_pop(L, 1);  // drop the error object
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_pop(L, 1);  // drop the error object
        return false;
    }
    return true;
}

bool lua_to_json(const shd::object& value, nlohmann::json* out) {
    if (!out) {
        return true;
    }

    if (!value.valid() ||     // GCOVR_EXCL_BR_LINE (defensive: always valid)
        value == shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: a service-table
                              // index always yields a valid object)
        *out = nullptr;
        return true;
    }
    if (value.is<bool>()) {
        *out = value.as<bool>();
        return true;
    }
    // Check string BEFORE number: lua_isnumber() returns true for strings
    // that look like numbers (e.g. "8194"), but we want to preserve the
    // original string type in JSON (legacy parity: is<string> has priority).
    if (value.is<std::string>()) {
        *out = value.as<std::string>();
        return true;
    }
    if (value.is<double>()) {
        // The legacy binding's is<int64_t>() accepts any Lua number when
        // the legacy default did not check precision, so checking it
        // first would let as<int64_t>() truncate floats (e.g. 3.14 -> 3).
        // Read as double, then only round-trip through int64_t when the
        // value is a whole number so the JSON keeps its original type.
        const double d = value.as<double>();
        const auto as_int = static_cast<std::int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            *out = as_int;
        } else {
            *out = d;
        }
        return true;
    }
    // Client identity userdata travels through message payloads in its
    // marker form (the inverse of the json_to_lua materialization).
    if (value.is<ClientContextBox>()) {
        *out = box_context_marker(value);
        return true;
    }
    if (value.is<ClientRefBox>()) {
        *out = box_context_marker(value);
        return true;
    }
#ifdef SHIELD_ENABLE_PLAYER
    // PlayerRef userdata travels in its marker form (the inverse of the
    // json_to_lua materialization).
    if (value.is<PlayerRefBox>()) {
        *out = box_player_marker(value);
        return true;
    }
#endif
    if (!value.is<shd::table>()) {
        *out = "<unsupported>";
        return false;
    }

    shd::table table = value.as<shd::table>();
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;
    for (const auto& [key, _] : table) {
        ++entry_count;
        shd::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }
        // Read the full Lua integer and reject anything that would be
        // truncated by the int index below. Lua integers are 64-bit; a
        // table key larger than INT_MAX is not a valid dense-array index
        // and must not silently wrap to a small positive value.
        const int64_t index = key_obj.as<int64_t>();
        if (index <= 0 || index > std::numeric_limits<int>::max()) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        // An unsupported child already wrote the "<unsupported>" sentinel
        // into its own out slot; embed it in the parent structure instead of
        // bailing with an unwritten parent (which the caller would read as
        // null and silently lose the whole return value).
        nlohmann::json array = nlohmann::json::array();
        bool all_supported = true;
        for (std::size_t i = 1; i <= max_index; ++i) {
            nlohmann::json item;
            if (!lua_to_json(table[static_cast<int>(i)], &item)) {
                all_supported = false;
            }
            array.push_back(std::move(item));
        }
        *out = std::move(array);
        return all_supported;
    }

    nlohmann::json object = nlohmann::json::object();
    bool all_supported = true;
    for (const auto& [key, val] : table) {
        shd::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int64_t>());
        } else {
            continue;
        }

        nlohmann::json item;
        if (!lua_to_json(val, &item)) {
            all_supported = false;
        }
        object[object_key] = std::move(item);
    }
    *out = std::move(object);
    return all_supported;
}

// Convenience wrapper that returns the JSON value directly.
nlohmann::json lua_to_json(const shd::object& value) {
    nlohmann::json result;
    if (!lua_to_json(value, &result)) {
        return "<unsupported>";
    }
    return result;
}

bool LuaRuntime::load_service_module(std::shared_ptr<LuaVM> vm,
                                     std::string_view script_path,
                                     std::string* error) {
    try {
        shd::state& lua = *vm->state();
        const std::string path_str(script_path);

        // Try to load from cache
        std::string source_code;
        bool use_cache = false;

        if (impl_->cache_config.enabled) {
            std::lock_guard<std::mutex> lock(impl_->cache_mutex);

            const int64_t current_mtime = impl_->get_file_mtime(path_str);
            auto it = impl_->script_cache.find(path_str);

            if (it != impl_->script_cache.end() &&
                impl_->is_cache_valid(it->second, current_mtime)) {
                // Cache hit
                source_code = it->second.bytecode;
                it->second.last_used = impl_->current_time_ms();
                use_cache = true;
            }
        }

        // Load from file if cache miss or disabled
        if (!use_cache) {
            // Read file content
            try {
                std::ifstream file(path_str);
                if (!file.is_open()) {
                    if (error) {
                        *error = "Failed to open file: " + path_str;
                    }
                    return false;
                }
                std::stringstream buffer;
                buffer << file.rdbuf();
                source_code = buffer.str();
                // GCOVR_EXCL_START (catch-entry pseudo-arc: the stream never
                // throws here — open() already failed above and a read miss
                // only sets failbit, so the handler entry arc starves; keep the
                // catch clause on one physical line so the entry stays in this
                // region)
            } catch (const std::exception& e) {
                if (error) {
                    *error = "Failed to read file: " + std::string(e.what());
                }
                return false;
            }
            // GCOVR_EXCL_STOP

            // Update cache if enabled
            if (impl_->cache_config.enabled) {
                std::lock_guard<std::mutex> lock(impl_->cache_mutex);

                ScriptCacheEntry entry;
                entry.bytecode = source_code;
                entry.mtime = impl_->get_file_mtime(path_str);
                entry.last_used = impl_->current_time_ms();

                impl_->evict_if_needed();
                impl_->script_cache[path_str] = std::move(entry);
            }
        }

        // Load and execute the source code. The chunkname carries the '@'
        // file prefix (what luaL_loadfile would pass): with a bare path Lua
        // takes the *string* truncation for short_src — head kept, tail cut —
        // which drops the script file name once the path exceeds LUA_IDSIZE
        // (60). Every Windows runner temp path does (C:\Users\... ≈ 62
        // chars), and profile hotspot attribution then cannot see the
        // service file at all (ProfileHappyPathStartStatusStop, 2026-09-23).
        // The '@' prefix routes the name through the file truncation, which
        // keeps the tail — the file name survives any path length.
        shd::load_result loaded = lua.load(source_code, "@" + path_str);
        if (!loaded.valid()) {
            shd::error err = loaded;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        shd::protected_function chunk = loaded;
        shd::protected_function_result result = chunk();
        if (!result.valid()) {
            shd::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        shd::object module = result;
        if (!module.is<shd::table>()) {
            if (error) {
                *error = "service module must return a table";
            }
            return false;
        }

        vm->service_table(module.as<shd::table>());
        return true;
    } catch (const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: catch-entry pseudo-arc)
                                         // GCOVR_EXCL_START (unreachable:
        if (error) {  // GCOVR_EXCL_BR_LINE (the module type was checked above
                      // and as<shd::table>
            *error = e.what();  // GCOVR_EXCL_BR_LINE (compiler artifact) is the
                                // only remaining throwing call)
        }
        return false;
    }
    // GCOVR_EXCL_STOP
}

bool LuaRuntime::register_api(std::shared_ptr<LuaVM> vm, std::string* error) {
    // Register all shield.* API functions
    register_full_shield_api(vm->state()->lua_state(), impl_->service_manager,
                             this);

    // Inject plugin Lua search paths and dispatch register_lua on every
    // started plugin instance. Each VM gets its own bindings; the host uses
    // a thread-local pointer to expose the active L to plugins that query
    // host_api.lua_state() / lua_add_path() during register_lua.
    lua_State* L = vm->state()->lua_state();
    auto& host = shield::plugin::global_host();
    host.inject_lua_paths(L);
    std::string lua_err;
    if (!host.register_lua_all(L, lua_err)) {
        // GCOVR_EXCL_START (register_lua_all failure at service VM setup
        // needs a native plugin whose register_lua fails; integration
        // context)
        SHIELD_LOG_WARNING(
            shield::log::get_logger("lua"),
            std::string("plugin register_lua failed: ") + lua_err);
        if (error)
            *error = lua_err;  // GCOVR_EXCL_BR_LINE (integration: needs a
                               // native plugin whose register_lua fails at
                               // service VM setup)
        return false;
        // GCOVR_EXCL_STOP
    }
    return true;
}

void LuaRuntime::set_service_manager(LuaServiceManager* manager) {
    impl_->service_manager = manager;
}

LuaServiceManager* LuaRuntime::service_manager() const {
    return impl_->service_manager;
}

std::string LuaRuntime::get_global(std::shared_ptr<LuaVM> vm,
                                   std::string_view name) {
    shd::state& lua = *vm->state();
    shd::object value = lua[name];

    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    if (value.is<double>()) {
        const double d = value.as<double>();
        const auto as_int = static_cast<int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            return std::to_string(as_int);
        }
        return std::to_string(d);
    }
    return "";
}

void LuaRuntime::set_global(std::shared_ptr<LuaVM> vm, std::string_view name,
                            std::string_view value) {
    shd::state& lua = *vm->state();
    lua[name] = std::string(value);
}

lua_State* LuaRuntime::vm_main_state(std::shared_ptr<LuaVM> vm) {
    if (!vm ||  // GCOVR_EXCL_BR_LINE (defensive: Impl::state() is never
        !vm->state()) {  // GCOVR_EXCL_BR_LINE (defensive: null state only
                         // null on a live VM; mid-teardown)
        return nullptr;
    }
    return vm->state()->lua_state();
}

bool LuaRuntime::exec_lua(std::shared_ptr<LuaVM> vm, const std::string& code,
                          nlohmann::json* result, std::string* error) {
    if (!vm || !vm->state()) {  // GCOVR_EXCL_BR_LINE (defensive: Impl::state()
                                // is never null on a live VM)
        if (error) *error = "invalid VM";
        return false;
    }

    shd::state& lua = *vm->state();
    lua_State* L = lua.lua_state();

    // Compile
    int load_status = luaL_loadbuffer(L, code.c_str(), code.size(), "=console");
    if (load_status != LUA_OK) {
        if (error) {
            *error = lua_tostring(L, -1)
                         ? lua_tostring(L, -1)
                         : "load error";  // GCOVR_EXCL_BR_LINE (defensive: a
                                          // failed compile always pushes a
                                          // string error message)
        }
        lua_pop(L, 1);
        return false;
    }

    // Execute
    int base = lua_gettop(L);  // function is at base
    int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (call_status != LUA_OK) {
        if (error) {
            *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "exec error";
        }
        lua_pop(L, 1);
        return false;
    }

    // Capture return values
    int nresults = lua_gettop(L) - base + 1;
    if (result && nresults > 0) {
        *result = nlohmann::json::array();
        shd::state_view sv(L);
        for (int i = base; i <= lua_gettop(L); ++i) {
            shd::stack_object so(sv, i);
            shd::object obj = so;
            if (obj.is<shd::nil_t>()) {
                result->push_back(nullptr);
            } else if (obj.is<bool>()) {
                result->push_back(obj.as<bool>());
            } else if (obj.is<double>()) {
                // The legacy binding's is<int64_t>() accepts any Lua number
                // when SOL_NUMBER_PRECISION_CHECKS is off (the default), so
                // checking it first would let as<int64_t>() truncate floats
                // (e.g. 2.5 -> 2). Read as double, then only round-trip through
                // int64_t when the value is a whole number so the JSON keeps
                // its original type. Mirrors lua_to_json above.
                const double d = obj.as<double>();
                const auto as_int = static_cast<std::int64_t>(d);
                if (static_cast<double>(as_int) == d) {
                    result->push_back(as_int);
                } else {
                    result->push_back(d);
                }
            } else if (obj.is<std::string>()) {
                result->push_back(obj.as<std::string>());
            } else {
                // For tables, functions, etc. - convert to string via Lua
                // tostring. The call must stay protected: __tostring
                // metamethods (and the tostring global itself) are
                // user-controllable in the eval VMs, and since the panic
                // handler aborts, an unprotected lua_call here would kill
                // the process instead of degrading this one result slot.
                // (luaL_tolstring is not an alternative: it raises
                // unprotected through luaL_callmeta/luaL_error.)
                lua_getglobal(L, "tostring");
                lua_pushvalue(L, i);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                    const char* s = lua_tostring(L, -1);
                    result->push_back(
                        s ? std::string(s)  // GCOVR_EXCL_BR_LINE (defensive: a
                                            // successful tostring pcall always
                                            // leaves a string)
                          : "");  // GCOVR_EXCL_BR_LINE (defensive: a successful
                                  // tostring pcall always leaves a string, so s
                                  // is never null; other arcs are EH edges)
                } else {
                    // Degrade: carry the failure in the slot instead of
                    // failing the whole exec (the chunk already succeeded).
                    const char* emsg = lua_tostring(L, -1);
                    result->push_back(
                        std::string("<tostring error: ") +
                        (emsg != nullptr ? emsg : "non-string error") + ">");
                }
                lua_pop(L, 1);
            }
        }
    }
    // Clean up stack
    lua_settop(L, base - 1);
    return true;
}

// LuaRuntime cache management methods
void LuaRuntime::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    impl_->script_cache.clear();
}

size_t LuaRuntime::cache_size() const {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    return impl_->script_cache.size();
}

LuaCacheConfig LuaRuntime::cache_config() const { return impl_->cache_config; }

}  // namespace shield::lua
