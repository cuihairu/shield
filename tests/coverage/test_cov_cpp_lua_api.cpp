// Coverage tests for src/lua/lua_api.cpp
#define BOOST_TEST_MODULE CovCppLuaApi
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

using namespace shield::lua;

// Defined in lua_api.cpp but not declared in the public header; declare it
// here so the table-based overload gets covered too.
namespace shield::lua::api {
void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime);
}

namespace {

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::string write_script(const std::string& path, const char* content) {
    if (const auto slash = path.find_last_of("/\\");
        slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
    }
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return path;
}

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json extra = nlohmann::json::object()) {
    auto opts = nlohmann::json{
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        opts[it.key()] = it.value();
    }
    return opts;
}

// ---------------------------------------------------------------------------
// Mock session with injectable send failures.
// ---------------------------------------------------------------------------
class MockSession final : public shield::net::Session {
public:
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote,
                bool protocol_enabled = false, std::string codec = "json")
        : id_(id),
          remote_(std::move(remote)),
          protocol_enabled_(protocol_enabled),
          codec_(std::move(codec)) {}

    shield::net::SessionId id() const override { return id_; }
    shield::net::RemoteAddress remote_addr() const override { return remote_; }
    bool send(const std::vector<uint8_t>& data,
              std::string* error = nullptr) override {
        if (!alive_) {
            if (error) *error = "session is closed";
            return false;
        }
        if (fail_raw_send_) {
            if (error) *error = raw_send_error_;
            return false;
        }
        sent_.push_back(data);
        return true;
    }
    void close(std::string reason) override {
        alive_ = false;
        close_reason_ = std::move(reason);
    }
    bool is_alive() const override { return alive_; }
    std::string error_code() const override {
        return alive_ ? "" : "session_closed";
    }
    bool has_protocol_pipeline() const override { return protocol_enabled_; }
    std::string_view protocol_codec_name() const override {
        return protocol_enabled_ ? std::string_view(codec_)
                                 : std::string_view{};
    }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error) override {
        if (!alive_) {
            if (error) *error = "session is closed";
            return false;
        }
        if (fail_msg_send_) {
            if (error) *error = msg_send_error_;
            return false;
        }
        sent_messages_.push_back(message);
        return true;
    }
    void set_user_data(std::string key, std::string value) override {
        user_data_[std::move(key)] = std::move(value);
    }
    std::string get_user_data(std::string_view key) const override {
        auto it = user_data_.find(std::string(key));
        return it == user_data_.end() ? "" : it->second;
    }
    shield::net::SessionBinding binding() const override { return binding_; }
    void reset_binding(shield::net::SessionBinding initial) override {
        binding_ = std::move(initial);
    }
    bool apply_binding(std::string target_service, std::string player_id,
                       uint32_t expected_epoch,
                       shield::net::SessionBinding* out) override {
        if (expected_epoch != shield::net::kAnyEpoch &&
            expected_epoch != binding_.epoch) {
            return false;
        }
        binding_.target_service = std::move(target_service);
        binding_.player_id = std::move(player_id);
        ++binding_.epoch;
        if (out != nullptr) {
            *out = binding_;
        }
        return true;
    }

    void set_raw_send_failure(std::string error) {
        fail_raw_send_ = true;
        raw_send_error_ = std::move(error);
    }
    void set_msg_send_failure(std::string error) {
        fail_msg_send_ = true;
        msg_send_error_ = std::move(error);
    }

    const std::vector<std::vector<uint8_t>>& sent() const { return sent_; }
    const std::vector<shield::transport::DecodedBody>& sent_messages() const {
        return sent_messages_;
    }
    const std::string& close_reason() const { return close_reason_; }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool protocol_enabled_ = false;
    std::string codec_;
    bool alive_ = true;
    std::string close_reason_;
    bool fail_raw_send_ = false;
    std::string raw_send_error_;
    bool fail_msg_send_ = false;
    std::string msg_send_error_;
    std::vector<std::vector<uint8_t>> sent_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
    std::unordered_map<std::string, std::string> user_data_;
    shield::net::SessionBinding binding_;
};

// ---------------------------------------------------------------------------
// Minimal blocking HTTP responder used to exercise shield.http response
// handling (including JSON auto-parse) without external network access.
// ---------------------------------------------------------------------------
#ifndef _WIN32  // raw POSIX sockets: Linux/macOS only
class MiniHttpServer {
public:
    MiniHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                          &len) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~MiniHttpServer() {
        stopped_ = true;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }

private:
    static size_t find_header_end(const std::string& req) {
        return req.find("\r\n\r\n");
    }

    static size_t content_length(const std::string& headers) {
        const std::string needle = "content-length:";
        std::string lower;
        lower.reserve(headers.size());
        for (char c : headers)
            lower.push_back(
                static_cast<char>(::tolower(static_cast<unsigned char>(c))));
        size_t pos = lower.find(needle);
        if (pos == std::string::npos) return 0;
        size_t val = 0;
        size_t i = pos + needle.size();
        while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;
        while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
            val = val * 10 + static_cast<size_t>(lower[i] - '0');
            ++i;
        }
        return val;
    }

    void run() {
        while (!stopped_) {
            int conn = ::accept(listen_fd_, nullptr, nullptr);
            if (conn < 0) break;
            handle(conn);
            ::close(conn);
        }
    }

    void handle(int conn) {
        std::string req;
        char buf[4096];
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 1u << 20) return;
            header_end = find_header_end(req);
        }
        const size_t body_need = content_length(req.substr(0, header_end));
        while (req.size() < header_end + 4 + body_need) {
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, static_cast<size_t>(n));
        }

        std::string path = "/";
        const size_t sp1 = req.find(' ');
        const size_t sp2 =
            req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        std::string body = "plain";
        std::string ctype = "text/plain";
        std::string extra_ct = "Content-Type: " + ctype + "\r\n";
        if (path.find("goodjson") != std::string::npos) {
            body =
                "{\"str\":\"s\",\"i\":1,\"f\":1.5,\"u\":3000000000,"
                "\"b\":true,\"arr\":[1,2],\"obj\":{\"k\":\"v\"}}";
            ctype = "application/json";
            extra_ct = "Content-Type: " + ctype + "\r\n";
        } else if (path.find("badjson") != std::string::npos) {
            body = "{definitely not json";
            ctype = "application/json";
            extra_ct = "Content-Type: " + ctype + "\r\n";
        } else if (path.find("ctjson") != std::string::npos) {
            // Content-Type is not JSON, but the body starts with '{' so the
            // heuristic parse should kick in.
            body = "{\"ok\":true}";
            ctype = "text/plain";
            extra_ct = "Content-Type: " + ctype + "\r\n";
        } else if (path.find("noct") != std::string::npos) {
            // No Content-Type header at all; JSON detected from the body.
            body = "{\"x\":1}";
            extra_ct.clear();
        }

        const std::string resp =
            "HTTP/1.1 200 OK\r\n" + extra_ct +
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < resp.size()) {
            ssize_t n = ::send(conn, resp.data() + sent, resp.size() - sent,
                               MSG_NOSIGNAL);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopped_{false};
    std::thread thread_;
};
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Lua service scripts used by the interaction tests.
// ---------------------------------------------------------------------------
const char* kServiceB = R"lua(
local M = {}
local state = {}
function M.on_init(args) end
function M.ping(ctx) return "pong" end
function M.who(ctx) return shield.sender(), ctx.sender end
function M.sleepy(ctx)
  shield.sleep(150)
  return "awake"
end
function M.thrower(ctx) error("boom") end
function M.record(ctx, tag, value) state[tag] = value return true end
function M.get(ctx, tag) return state[tag] end
return M
)lua";

const char* kServiceA = R"lua(
local M = {}
local state = { forked = 0 }
local reporter = nil

function M.on_init(args) end

function M.set_reporter(ctx, name) reporter = name return true end

function M.who_calls(ctx)
  return shield.sender(), ctx.sender
end

function M.self_id(ctx)
  local h = shield.self()
  if not h then return nil end
  return h:id()
end

function M.trace_of(ctx) return shield.trace() end
function M.deadline_of(ctx) return shield.deadline() end

function M.names(ctx) return shield.names() end

function M.query_alias(ctx, name)
  local h, err = shield.query(name)
  if not h then return nil, err and err.code or nil end
  return h:id()
end

function M.reg(ctx, name)
  local ok, err = shield.register(name)
  return ok, err and err.code or nil
end

function M.unreg(ctx, name)
  local ok, err = shield.unregister(name)
  return ok, err and err.code or nil
end

function M.send_on_reserved(ctx, target)
  local ok, err = shield.send(target, "on_reserved")
  return ok, err and err.code or nil
end

function M.send_dead(ctx, target)
  local ok, err = shield.send(target, "ping")
  return ok, err and err.code or nil
end

function M.send_huge(ctx, target)
  local ok, err = shield.send(target, "ping", string.rep("x", 2200000))
  return ok, err and err.code or nil
end

function M.send_func(ctx, target)
  local ok, err = shield.send(target, "ping", function() end)
  return ok, err and err.code or nil
end

function M.call_ok(ctx, target)
  local ok, v = shield.call(target, "ping")
  return ok, v
end

function M.call_timeout_short(ctx, target)
  local ok, err = shield.call_timeout(30, target, "sleepy")
  return ok, err and err.code or nil
end

function M.call_who(ctx, target)
  local ok, sender, ctx_sender = shield.call(target, "who")
  return ok, sender, ctx_sender
end

-- Coroutine call path with a custom timeout budget on the happy path.
function M.call_timeout_ok(ctx, target)
  local ok, v = shield.call_timeout(2000, target, "ping")
  return ok, v
end

-- Call error mapping seen from Lua inside a handler.
function M.call_err(ctx, target, method)
  local ok, err = shield.call(target, method)
  if ok then return true end
  return false, err and err.code or nil
end

-- send/call through a ServiceHandle userdata target from a handler.
function M.handle_roundtrip(ctx, target)
  local h, qerr = shield.query(target)
  if not h then return false, false, qerr and qerr.code or nil end
  local sent = shield.send(h, "ping", "via_handle")
  local ok, v = shield.call(h, "ping")
  return sent == true, ok, v
end

function M.spawn_array_opts(ctx, module)
  local h, err = shield.spawn(module, {1, 2})
  if not h then return false, err and err.code or nil end
  return true
end

function M.do_fork(ctx)
  shield.fork(function() state.forked = state.forked + 1 end)
  return true
end

function M.forked_count(ctx) return state.forked end

function M.timer_once_cancel(ctx)
  local id = shield.timer_once(4000, function() end)
  if id == 0 then return false, "zero_id" end
  local ok, err = shield.cancel_timer(id)
  return ok, err and err.code or nil
end

function M.timer_repeat_cancel(ctx)
  local id = shield.timer(4000, function() end)
  if id == 0 then return false, "zero_id" end
  return shield.cancel_timer(id)
end

function M.sleep_once(ctx)
  shield.sleep(20)
  return "slept_once"
end

function M.sleep_then_call(ctx, target)
  shield.sleep(20)
  -- After the sleep timer resumes this handler it yields again inside the
  -- call; the callee's response completes the resumed segment.
  local ok, v = shield.call_timeout(2000, target, "ping")
  state.sleep_call_ok = ok
  state.sleep_call_err = v or nil
  -- Same shape with a budget shorter than the callee's own sleep: the call
  -- timeout driver resumes the suspended handler with the timeout error.
  local ok2, err2 = shield.call_timeout(50, target, "sleepy")
  state.sleep_call2_ok = ok2
  state.sleep_call2_err = err2 and err2.code or nil
  return ok
end

function M.get_sleep_call(ctx)
  return state.sleep_call_ok, state.sleep_call_err,
      state.sleep_call2_ok, state.sleep_call2_err
end

function M.self_exit(ctx)
  shield.exit("cov_exit")
  return "bye"
end

function M.panic_now(ctx)
  shield.panic("cov_panic")
  return "panicking"
end

function M.on_panic(reason, info)
  if reporter then
    pcall(function() shield.send(reporter, "record", "panic", reason) end)
  end
end

function M.on_exit(reason)
  if reporter then
    pcall(function()
      shield.send(reporter, "record", "in_exit", shield._is_in_exit())
    end)
  end
end

return M
)lua";

const char* kInitFail = R"lua(
local M = {}
function M.on_init(args) error("no dice") end
return M
)lua";

const char* kInitSlowFail = R"lua(
local M = {}
function M.on_init(args)
  shield.sleep(250)
  error("slow failure")
end
return M
)lua";

const char* kChild = R"lua(
local M = {}
function M.on_init(args) end
function M.ping(ctx) return "child_pong" end
return M
)lua";

bool run_script(sol::state& lua, const std::string& code) {
    auto result = lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error e = result;
        std::fprintf(stderr, "lua error: %s\n", e.what());
        return false;
    }
    return true;
}

}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(CovCppLuaApi)

// ---------------------------------------------------------------------------
// Direct json_to_lua conversion branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(JsonToLuaTypeBranches) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string);

    lua["v_nil"] = json_to_lua(lua, nlohmann::json());
    BOOST_CHECK(run_script(lua, "assert(v_nil == nil)"));

    lua["v_bool"] = json_to_lua(lua, nlohmann::json(true));
    BOOST_CHECK(lua["v_bool"].get_type() == sol::type::boolean);

    lua["v_int"] = json_to_lua(lua, nlohmann::json(42));
    BOOST_CHECK(lua["v_int"].get_type() == sol::type::number);

    lua["v_uint"] = json_to_lua(lua, nlohmann::json(18000000000000000000ULL));
    BOOST_CHECK(lua["v_uint"].get_type() == sol::type::number);

    lua["v_float"] = json_to_lua(lua, nlohmann::json(1.5));
    BOOST_CHECK(lua["v_float"].get_type() == sol::type::number);

    lua["v_str"] = json_to_lua(lua, nlohmann::json("hello"));
    BOOST_CHECK(lua["v_str"].get_type() == sol::type::string);

    lua["v_arr"] = json_to_lua(lua, nlohmann::json::parse("[1,[2,3]]"));
    BOOST_CHECK(lua["v_arr"].get_type() == sol::type::table);
    BOOST_CHECK(run_script(lua, "assert(#v_arr == 2 and #v_arr[2] == 2)"));

    lua["v_obj"] =
        json_to_lua(lua, nlohmann::json::parse(R"({"a":1,"b":"x"})"));
    BOOST_CHECK(lua["v_obj"].get_type() == sol::type::table);
    BOOST_CHECK(run_script(lua, "assert(v_obj.a == 1 and v_obj.b == 'x')"));

    // Client-identity marker in a VM without the identity factory installed
    // (no register_full_shield_api here): stays a plain table.
    nlohmann::json marker =
        shield::lua::ClientContextData{"gw-1", 1, 3, "player-1", "json"}
            .to_json();
    lua["v_marker"] = json_to_lua(lua, marker);
    BOOST_CHECK(lua["v_marker"].get_type() == sol::type::table);
    BOOST_CHECK(run_script(lua, "assert(v_marker.player_id == 'player-1')"));

    // A marker-shaped object with a wrong flag value is NOT an identity:
    // it stays a plain table.
    nlohmann::json fake = nlohmann::json::object(
        {{"__shield_client_ref", false}, {"session_id", 1}});
    lua["v_fake"] = json_to_lua(lua, fake);
    BOOST_CHECK(lua["v_fake"].get_type() == sol::type::table);
}

// ---------------------------------------------------------------------------
// Namespace-level registration stubs.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegistrationStubs) {
    LuaRuntime runtime;
    register_shield_api(runtime);
    api::register_service_api(runtime);
    api::register_message_api(runtime);
    api::register_timer_api(runtime);
    api::register_task_api(runtime);
    api::register_config_api(runtime);
    api::register_log_api(runtime);
    api::register_gateway_api(runtime);

    sol::state lua;
    lua.open_libraries(sol::lib::base);
    sol::table table = lua.create_table();
    api::register_timer_api(table, nullptr, nullptr);
    BOOST_CHECK(true);
}

// ---------------------------------------------------------------------------
// Main-thread (no dispatch context) API surface: self/exit/query/register
// wrappers, invalid targets, spawn failure modes, timer/call primitives
// that return early, config parsing branches and log functions.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MainThreadApiSurface) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // self outside a service context -> nil.
    BOOST_CHECK(run_script(lua, "assert(shield.self() == nil)"));

    // exit/panic outside a dispatch context are safe no-ops.
    BOOST_CHECK(run_script(lua, "shield.exit('from_main') shield.panic('x')"));

    // names lists nothing initially.
    BOOST_CHECK(run_script(lua, "assert(#shield.names() == 0)"));

    // query miss.
    BOOST_CHECK(
        run_script(lua,
                   "local h, err = shield.query('cov_ghost')\n"
                   "assert(h == nil and err.code == 'service_not_found')"));

    // register/unregister outside a service context fail.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.register('cov_alias')\n"
                   "assert(ok == false and err.code == 'register_failed')"));
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.unregister('cov_alias')\n"
                   "assert(ok == false and err.code == 'unregister_failed')"));

    // _make_handle builds a ServiceHandle userdata.
    BOOST_CHECK(run_script(lua,
                           "local h = shield._make_handle('cov_x')\n"
                           "assert(h:id() == 'cov_x')"));

    // Context probes outside dispatch.
    BOOST_CHECK(run_script(lua,
                           "assert(shield.sender() == nil)\n"
                           "assert(shield.trace() == nil)\n"
                           "assert(shield.deadline() == nil)\n"
                           "assert(shield._is_in_exit() == false)"));

    // Clock APIs.
    BOOST_CHECK(run_script(
        lua,
        "assert(type(shield.now()) == 'number')\n"
        "assert(type(shield.monotonic()) == 'number')\n"
        "assert(type(os.time()) == 'number')\n"
        "assert(type(os.date('%Y')) == 'string')\n"
        "assert(type(os.time({year=2020, month=1, day=1})) == 'number')\n"
        "assert(type(os.date('!*t', 100)) == 'table')"));

    // Blocking sleep on the main thread.
    BOOST_CHECK(run_script(lua, "shield.sleep(5)"));

    // _resume_after with a negative delay clamps to zero; no service
    // context so nothing is scheduled.
    BOOST_CHECK(run_script(lua, "shield._resume_after(-5)"));

    // Timers without a service context return id 0.
    BOOST_CHECK(
        run_script(lua,
                   "assert(shield.timer_once(10, function() end) == 0)\n"
                   "assert(shield.timer(10, function() end) == 0)"));

    // Cancelling an unknown timer fails with timer_not_found.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.cancel_timer(987654321)\n"
                   "assert(ok == false and err.code == 'timer_not_found')"));

    // _coro_spawn outside dispatch returns 0.
    BOOST_CHECK(run_script(
        lua, "assert(shield._coro_spawn('cov_missing', nil, 100) == 0)"));

    // _coro_call with an invalid / unknown target returns 0.
    BOOST_CHECK(run_script(
        lua,
        "assert(shield._coro_call(123, 'm', {}, 100) == 0)\n"
        "assert(shield._coro_call('cov_ghost', 'm', {}, 100) == 0)"));

    // shield.send / shield.call / shield.call_timeout invalid target type.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send(123, 'm')\n"
                   "assert(ok == false and err.code == 'invalid_target')"));
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.call(123, 'm')\n"
                   "assert(ok == false and err.code == 'invalid_target')"));
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.call_timeout(50, 123, 'm')\n"
                   "assert(ok == false and err.code == 'invalid_target')"));

    // shield.send to an unknown service. Off-coroutine, shield.call freezes
    // to call_not_allowed_off_coroutine (target shape is still validated
    // first, above).
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('cov_ghost', 'm')\n"
                   "assert(ok == false and err.code == 'service_not_found')"));
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.call('cov_ghost', 'm')\n"
                           "assert(ok == false and err.code == "
                           "'call_not_allowed_off_coroutine')"));

    // Log functions with and without a service prefix (no context here).
    BOOST_CHECK(run_script(lua,
                           "shield.log.debug('d')\n"
                           "shield.log.info({k = 1})\n"
                           "shield.log.warn('w')\n"
                           "shield.log.error(42)"));

    // _sync_spawn failure: missing module -> spawn_failed.
    BOOST_CHECK(run_script(
        lua,
        "local h, err = shield.spawn('/tmp/opencode/cov_does_not_exist.lua')\n"
        "assert(h == nil and err.code == 'spawn_failed')"));

    // _sync_spawn with array-like opts: options normalized to an object.
    BOOST_CHECK(run_script(
        lua,
        "local h, err = shield.spawn('/tmp/opencode/cov_does_not_exist.lua',"
        " {1, 2})\n"
        "assert(h == nil and err.code == 'spawn_failed')"));

    // spawn_timeout: on_init slower than the configured timeout.
    const auto slow =
        write_script("/tmp/opencode/cov_init_slow_fail.lua", kInitSlowFail);
    BOOST_CHECK(run_script(
        lua, "local h, err = shield.spawn('" + slow +
                 "', {name = 'cov_init_slow', timeout = 50})\n"
                 "assert(h == nil and err.code == 'spawn_timeout')"));

    // init_failed: on_init throws quickly.
    const auto initfail =
        write_script("/tmp/opencode/cov_init_fail.lua", kInitFail);
    BOOST_CHECK(
        run_script(lua, "local h, err = shield.spawn('" + initfail +
                            "', {name = 'cov_init_fail'})\n"
                            "assert(h == nil and err.code == 'init_failed')"));

    // Config parsing branches.
    auto& config = shield::config::global_config();
    config.set("cov.c.bool_t", std::string("true"));
    config.set("cov.c.bool_f", std::string("false"));
    config.set("cov.c.int", std::string("42"));
    config.set("cov.c.float", std::string("3.14"));
    config.set("cov.c.exp", std::string("1e3"));
    config.set("cov.c.inf", std::string("inf"));
    config.set("cov.c.partial_int", std::string("12abc"));
    config.set("cov.c.partial_float", std::string("3.14abc"));
    config.set("cov.c.text", std::string("hello"));
    BOOST_CHECK(run_script(
        lua,
        "assert(shield.config('cov.c.bool_t') == true)\n"
        "assert(shield.config('cov.c.bool_f') == false)\n"
        "assert(shield.config('cov.c.int') == 42)\n"
        "assert(shield.config('cov.c.float') == 3.14)\n"
        "assert(shield.config('cov.c.exp') == 1000.0)\n"
        "assert(shield.config('cov.c.inf') == math.huge)\n"
        "assert(shield.config('cov.c.partial_int') == '12abc')\n"
        "assert(shield.config('cov.c.partial_float') == '3.14abc')\n"
        "assert(shield.config('cov.c.text') == 'hello')\n"
        "assert(shield.config('cov.c.missing') == nil)\n"
        "assert(shield.config('cov.c.missing', 'fallback') == 'fallback')\n"
        "assert(shield.config('cov.c.missing', 7) == 7)"));
}

// ---------------------------------------------------------------------------
// Service-mediated coverage: context probes, register/unregister, coroutine
// call path, fork, timers, sleeps, exit and panic hooks.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ServiceInteractionPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script_a =
        write_script("/tmp/opencode/cov_service_a.lua", kServiceA);
    const auto script_b =
        write_script("/tmp/opencode/cov_service_b.lua", kServiceB);
    const auto child = write_script("/tmp/opencode/cov_child.lua", kChild);

    auto a = manager.spawn(script_a, opts_for("cov_a").dump());
    BOOST_REQUIRE(a.success);
    auto b = manager.spawn(script_b, opts_for("cov_b").dump());
    BOOST_REQUIRE(b.success);

    const nlohmann::json no_args = nlohmann::json::array();

    // Sender identity inside a handler dispatched from another service.
    BOOST_REQUIRE(manager.send(a.service_id, "set_reporter",
                               nlohmann::json::array({"cov_b"})));
    CallResult who = manager.call(a.service_id, "who_calls", no_args);
    BOOST_REQUIRE(who.success);
    BOOST_REQUIRE_EQUAL(who.values.size(), 2u);
    BOOST_CHECK(who.values[0].is_null() ||
                who.values[0].get<std::string>().empty());

    // shield.call from a handler (coroutine path) returns callee values.
    CallResult call_ok =
        manager.call(a.service_id, "call_ok", nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(call_ok.success);
    BOOST_REQUIRE_EQUAL(call_ok.values.size(), 2u);
    BOOST_CHECK_EQUAL(call_ok.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(call_ok.values[1].get<std::string>(), "pong");

    // The callee of a coroutine call observes the calling service as its
    // sender (shield.sender() non-nil on the callee side).
    CallResult cw = manager.call(a.service_id, "call_who",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(cw.success);
    BOOST_REQUIRE_EQUAL(cw.values.size(), 3u);
    BOOST_CHECK_EQUAL(cw.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(cw.values[1].get<std::string>(), "cov_a");

    // Coroutine call path with a custom timeout budget (happy path).
    CallResult ct_ok = manager.call(a.service_id, "call_timeout_ok",
                                    nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(ct_ok.success);
    BOOST_CHECK_EQUAL(ct_ok.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(ct_ok.values[1].get<std::string>(), "pong");

    // Call error mapping seen from Lua inside a handler: method_not_found /
    // handler_error.
    CallResult err_missing =
        manager.call(a.service_id, "call_err",
                     nlohmann::json::array({"cov_b", "missing_method"}));
    BOOST_REQUIRE(err_missing.success);
    BOOST_CHECK_EQUAL(err_missing.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(err_missing.values[1].get<std::string>(),
                      "method_not_found");
    CallResult err_throw = manager.call(
        a.service_id, "call_err", nlohmann::json::array({"cov_b", "thrower"}));
    BOOST_REQUIRE(err_throw.success);
    BOOST_CHECK_EQUAL(err_throw.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(err_throw.values[1].get<std::string>(), "handler_error");

    // send/call through a ServiceHandle userdata target from a handler.
    CallResult handle_rt = manager.call(a.service_id, "handle_roundtrip",
                                        nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(handle_rt.success);
    BOOST_CHECK_EQUAL(handle_rt.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(handle_rt.values[1].get<bool>(), true);
    BOOST_CHECK_EQUAL(handle_rt.values[2].get<std::string>(), "pong");

    // The coroutine call ran with cov_a as the sender on the callee side.
    CallResult trace = manager.call(a.service_id, "trace_of", no_args);
    BOOST_REQUIRE(trace.success);
    BOOST_CHECK(trace.values[0].is_null());
    CallResult deadline = manager.call(a.service_id, "deadline_of", no_args);
    BOOST_REQUIRE(deadline.success);
    BOOST_CHECK(deadline.values[0].is_null());

    // shield.self inside a handler returns the service's own handle.
    CallResult self_id = manager.call(a.service_id, "self_id", no_args);
    BOOST_REQUIRE(self_id.success);
    BOOST_CHECK_EQUAL(self_id.values[0].get<std::string>(), "cov_a");

    // names() inside a handler.
    CallResult names = manager.call(a.service_id, "names", no_args);
    BOOST_REQUIRE(names.success);
    {
        BOOST_REQUIRE(names.values.is_array() && names.values.size() == 1u &&
                      names.values[0].is_array());
        bool has_a = false;
        bool has_b = false;
        for (const auto& n : names.values[0]) {
            if (n.get<std::string>() == "cov_a") has_a = true;
            if (n.get<std::string>() == "cov_b") has_b = true;
        }
        BOOST_CHECK(has_a && has_b);
    }

    // query success + failure inside a handler.
    CallResult q1 = manager.call(a.service_id, "query_alias",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(q1.success);
    BOOST_CHECK_EQUAL(q1.values[0].get<std::string>(), "cov_b");
    CallResult q2 = manager.call(a.service_id, "query_alias",
                                 nlohmann::json::array({"cov_none"}));
    BOOST_REQUIRE(q2.success);
    BOOST_CHECK(q2.values[0].is_null());
    BOOST_CHECK_EQUAL(q2.values[1].get<std::string>(), "service_not_found");

    // register: success, duplicate, invalid; unregister: success, missing.
    CallResult r1 = manager.call(a.service_id, "reg",
                                 nlohmann::json::array({"cov_alias_a"}));
    BOOST_REQUIRE(r1.success);
    BOOST_CHECK_EQUAL(r1.values[0].get<bool>(), true);
    CallResult r2 =
        manager.call(a.service_id, "reg", nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(r2.success);
    BOOST_CHECK_EQUAL(r2.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(r2.values[1].get<std::string>(), "register_failed");
    CallResult r3 =
        manager.call(a.service_id, "reg", nlohmann::json::array({"bad name!"}));
    BOOST_REQUIRE(r3.success);
    BOOST_CHECK_EQUAL(r3.values[0].get<bool>(), false);
    CallResult r4 = manager.call(a.service_id, "unreg",
                                 nlohmann::json::array({"cov_alias_a"}));
    BOOST_REQUIRE(r4.success);
    BOOST_CHECK_EQUAL(r4.values[0].get<bool>(), true);
    CallResult r5 = manager.call(a.service_id, "unreg",
                                 nlohmann::json::array({"cov_alias_a"}));
    BOOST_REQUIRE(r5.success);
    BOOST_CHECK_EQUAL(r5.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(r5.values[1].get<std::string>(), "unregister_failed");
    CallResult r6 =
        manager.call(a.service_id, "unreg", nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(r6.success);
    BOOST_CHECK_EQUAL(r6.values[0].get<bool>(), false);

    // Send error mapping from inside a handler.
    CallResult s1 = manager.call(a.service_id, "send_on_reserved",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(s1.success);
    BOOST_CHECK_EQUAL(s1.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(s1.values[1].get<std::string>(), "invalid_method");

    auto c = manager.spawn(script_b, opts_for("cov_dead_svc").dump());
    BOOST_REQUIRE(c.success);
    manager.exit(c.service_id, "test");
    CallResult s2 = manager.call(a.service_id, "send_dead",
                                 nlohmann::json::array({c.service_id}));
    BOOST_REQUIRE(s2.success);
    BOOST_CHECK_EQUAL(s2.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(s2.values[1].get<std::string>(), "service_dead");

    CallResult s3 = manager.call(a.service_id, "send_huge",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(s3.success);
    BOOST_CHECK_EQUAL(s3.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(s3.values[1].get<std::string>(), "message_too_large");

    CallResult s4 = manager.call(a.service_id, "send_func",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(s4.success);
    BOOST_CHECK_EQUAL(s4.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(s4.values[1].get<std::string>(), "encode_failed");

    // Call error mapping: method missing on a live service, handler error.
    CallResult c1 =
        manager.call(a.service_id, "call_ok", nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(c1.success);
    auto main_missing = manager.call("cov_b", "missing_method", no_args);
    BOOST_CHECK(!main_missing.success);
    BOOST_CHECK(main_missing.error_message.find("method not found") !=
                std::string::npos);
    auto main_throw = manager.call("cov_b", "thrower", no_args);
    BOOST_CHECK(!main_throw.success);

    // Coroutine call timeout error code.
    CallResult t1 = manager.call(a.service_id, "call_timeout_short",
                                 nlohmann::json::array({"cov_b"}));
    BOOST_REQUIRE(t1.success);
    BOOST_REQUIRE(t1.values.is_array());
    BOOST_CHECK_EQUAL(t1.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(t1.values[1].get<std::string>(), "timeout");

    // shield.spawn through the coroutine path with array-like opts: the
    // opts normalization keeps the spawn parameters object-shaped; without
    // a valid name in the opts the spawn still fails deterministically.
    CallResult sp = manager.call(a.service_id, "spawn_array_opts",
                                 nlohmann::json::array({child}));
    BOOST_REQUIRE(sp.success);
    BOOST_REQUIRE(sp.values.is_array());
    BOOST_CHECK_EQUAL(sp.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(sp.values[1].get<std::string>(), "spawn_failed");

    // fork executes on the service actor.
    BOOST_REQUIRE(manager.call(a.service_id, "do_fork", no_args).success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult fc =
                manager.call(a.service_id, "forked_count", no_args, 1000);
            return fc.success && fc.values.is_array() &&
                   fc.values[0].get<int>() >= 1;
        },
        std::chrono::seconds(3)));

    // timer_once + cancel (success branch of cancel_timer).
    CallResult tc = manager.call(a.service_id, "timer_once_cancel", no_args);
    BOOST_REQUIRE(tc.success);
    BOOST_REQUIRE(tc.values.is_array());
    BOOST_CHECK_EQUAL(tc.values[0].get<bool>(), true);

    // repeating timer + cancel.
    CallResult tr = manager.call(a.service_id, "timer_repeat_cancel", no_args);
    BOOST_REQUIRE(tr.success);
    BOOST_REQUIRE(tr.values.is_array());
    BOOST_CHECK_EQUAL(tr.values[0].get<bool>(), true);

    // Single sleep in a sync-called handler: the sleep timer resumes the
    // coroutine to completion (LUA_OK resume branch).
    CallResult sl = manager.call(a.service_id, "sleep_once", no_args, 3000);
    BOOST_REQUIRE_MESSAGE(sl.success, sl.error_message);
    BOOST_CHECK_EQUAL(sl.values[0].get<std::string>(), "slept_once");

    // Sleep followed by coroutine calls: when the sleep timer resumes the
    // handler it yields again inside the call (LUA_YIELD resume branch).
    // The instant callee completes the first resumed segment; the short
    // budget against the sleepy callee completes the second deterministically
    // with the timeout code.
    BOOST_REQUIRE(manager.send(a.service_id, "sleep_then_call",
                               nlohmann::json::array({"cov_b"})));
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult v =
                manager.call(a.service_id, "get_sleep_call", no_args, 1000);
            return v.success && v.values.is_array() && v.values.size() == 4u &&
                   v.values[0].is_boolean() &&
                   v.values[0].get<bool>() == true &&
                   v.values[1].get<std::string>() == "pong" &&
                   v.values[2].is_boolean() &&
                   v.values[2].get<bool>() == false &&
                   v.values[3].get<std::string>() == "timeout";
        },
        std::chrono::seconds(3)));

    // shield.exit from a handler: on_exit reports _is_in_exit() == true.
    CallResult bye = manager.call(a.service_id, "self_exit", no_args);
    BOOST_REQUIRE(bye.success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult v = manager.call(
                b.service_id, "get", nlohmann::json::array({"in_exit"}), 1000);
            return v.success && v.values.is_array() && v.values.size() == 1u &&
                   v.values[0].is_boolean() && v.values[0].get<bool>() == true;
        },
        std::chrono::seconds(3)));

    // panic path: on_panic hook fires, then the service exits.
    auto p = manager.spawn(script_a, opts_for("cov_panic_svc").dump());
    BOOST_REQUIRE(p.success);
    BOOST_REQUIRE(manager.send(p.service_id, "set_reporter",
                               nlohmann::json::array({"cov_b"})));
    CallResult panicking = manager.call(p.service_id, "panic_now", no_args);
    BOOST_REQUIRE(panicking.success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult v = manager.call(b.service_id, "get",
                                        nlohmann::json::array({"panic"}), 1000);
            return v.success && v.values.is_array() && v.values.size() == 1u &&
                   v.values[0].is_string() &&
                   v.values[0].get<std::string>() == "cov_panic";
        },
        std::chrono::seconds(3)));
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult probe = manager.call(p.service_id, "ping", no_args, 500);
            return !probe.success;
        },
        std::chrono::seconds(3)));
}

// ---------------------------------------------------------------------------
// Runtime stopping: send/call/spawn surface the runtime_stopping code.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RuntimeStoppingCodes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script_b =
        write_script("/tmp/opencode/cov_service_b.lua", kServiceB);
    auto b = manager.spawn(script_b, opts_for("cov_stop_b").dump());
    BOOST_REQUIRE(b.success);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os);
    register_full_shield_api(lua, &manager, &runtime);

    manager.shutdown_all("cov_stopping");

    // send stays legal off-coroutine (fire-and-forget) and surfaces the
    // stopping state; spawn fails deterministically.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('cov_stop_b', 'ping')\n"
                   "assert(ok == false and err.code == 'runtime_stopping')"));
    BOOST_CHECK(
        run_script(lua, "local h, err = shield.spawn('" + script_b +
                            "')\n"
                            "assert(h == nil and err.code == 'spawn_failed')"));

    // shield.call is frozen off-coroutine (M4 semantics): a stopping runtime
    // does not re-enable suspending the caller thread.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.call('cov_stop_b', 'ping')\n"
                           "assert(ok == false and err.code == "
                           "'call_not_allowed_off_coroutine')"));

    // The stable error-code mapping for the stopping failure message stays
    // reachable for coroutine callers racing the shutdown window.
    BOOST_CHECK(
        run_script(lua,
                   "assert(shield._call_error_code('runtime is stopping') == "
                   "'runtime_stopping')\n"
                   "assert(shield._call_error_code('service not found: x') == "
                   "'service_not_found')"));
}

// ---------------------------------------------------------------------------
// Client identity userdata behaviour: marker materialization, read-only
// properties, ClientRef derivation, marker round-trip through lua_to_json,
// and from_json rejection branches (non-marker objects stay plain tables).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientIdentityBranches) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os);
    register_full_shield_api(lua, &manager, &runtime);

    // Full marker -> ClientContext userdata, plus ref() -> ClientRef.
    nlohmann::json marker =
        shield::lua::ClientContextData{"cov_gw", 201, 9, "player-201", "json"}
            .to_json();
    lua["ctx"] = json_to_lua(lua, marker);
    BOOST_CHECK(run_script(
        lua,
        "assert(ctx:session_id() == 201)\n"
        "assert(ctx:session_epoch() == 9)\n"
        "assert(ctx:player_id() == 'player-201')\n"
        "assert(ctx:gateway() == 'cov_gw')\n"
        "assert(ctx:protocol_profile_id() == 'json')\n"
        "local ref = ctx:ref()\n"
        "assert(ref:session_id() == 201 and ref:session_epoch() == 9)\n"
        "assert(ref:player_id() == 'player-201')"));

    // Both userdata kinds serialize back to the marker form when passed
    // through lua_to_json (message-argument transport).
    const nlohmann::json ctx_back = lua_to_json(lua["ctx"]);
    BOOST_CHECK(ctx_back.is_object());
    BOOST_CHECK(ctx_back["__shield_client_ref"].get<bool>());
    BOOST_CHECK_EQUAL(ctx_back["session_id"].get<uint64_t>(), 201u);
    BOOST_CHECK_EQUAL(ctx_back["session_epoch"].get<uint32_t>(), 9u);
    BOOST_CHECK_EQUAL(ctx_back["player_id"].get<std::string>(), "player-201");
    BOOST_CHECK_EQUAL(ctx_back["gateway_address"].get<std::string>(), "cov_gw");
    BOOST_CHECK_EQUAL(ctx_back["protocol_profile_id"].get<std::string>(),
                      "json");

    // The materializer global builds the same userdata directly (this is the
    // coroutine-resume path entry point used by push_json_to_stack).
    BOOST_CHECK(run_script(
        lua,
        "local made = __shield_make_client_context(77, 2, 'p-77', 'gw77', "
        "'raw')\n"
        "assert(made:session_id() == 77 and made:session_epoch() == 2)\n"
        "assert(made:player_id() == 'p-77')\n"
        "assert(made:gateway() == 'gw77')\n"
        "assert(made:protocol_profile_id() == 'raw')"));

    // Rejection branches: anything that is not a well-formed marker stays a
    // plain table (from_json returns nullopt).
    const nlohmann::json not_object = nlohmann::json::array({1, 2});
    lua["bad1"] = json_to_lua(lua, not_object);
    BOOST_CHECK(lua["bad1"].get_type() == sol::type::table);

    const nlohmann::json flag_false = nlohmann::json::object(
        {{"__shield_client_ref", false}, {"session_id", 1}});
    lua["bad2"] = json_to_lua(lua, flag_false);
    BOOST_CHECK(lua["bad2"].get_type() == sol::type::table);
    BOOST_CHECK(run_script(lua, "assert(bad2.session_id == 1)"));

    const nlohmann::json flag_wrong_type =
        nlohmann::json::object({{"__shield_client_ref", "yes"}});
    lua["bad3"] = json_to_lua(lua, flag_wrong_type);
    BOOST_CHECK(lua["bad3"].get_type() == sol::type::table);

    // Field-level type guards: wrong-typed fields fall back to defaults but
    // the marker is still materialized.
    const nlohmann::json wrong_fields =
        nlohmann::json::object({{"__shield_client_ref", true},
                                {"session_id", "not-a-number"},
                                {"session_epoch", -1},
                                {"player_id", 42},
                                {"gateway_address", true},
                                {"protocol_profile_id", std::vector<int>{1}}});
    lua["loose"] = json_to_lua(lua, wrong_fields);
    BOOST_CHECK(run_script(lua,
                           "assert(loose:session_id() == 0)\n"
                           "assert(loose:session_epoch() == 0)\n"
                           "assert(loose:player_id() == '')\n"
                           "assert(loose:gateway() == '')\n"
                           "assert(loose:protocol_profile_id() == '')"));

    // Marker with no optional fields at all: all defaults.
    const nlohmann::json bare =
        nlohmann::json::object({{"__shield_client_ref", true}});
    lua["bare_ctx"] = json_to_lua(lua, bare);
    BOOST_CHECK(run_script(
        lua,
        "assert(bare_ctx:session_id() == 0 and bare_ctx:session_epoch() == 0)\n"
        "assert(bare_ctx:player_id() == '' and bare_ctx:gateway() == '')"));

    // from_json direct unit coverage for the same branches.
    BOOST_CHECK(
        !shield::lua::ClientContextData::from_json(not_object).has_value());
    BOOST_CHECK(
        !shield::lua::ClientContextData::from_json(flag_false).has_value());
    BOOST_CHECK(!shield::lua::ClientContextData::from_json(flag_wrong_type)
                     .has_value());
    const auto loose_data =
        shield::lua::ClientContextData::from_json(wrong_fields);
    BOOST_REQUIRE(loose_data.has_value());
    BOOST_CHECK_EQUAL(loose_data->session_id, 0u);
    BOOST_CHECK_EQUAL(loose_data->session_epoch, 0u);
    BOOST_CHECK(loose_data->player_id.empty());
    BOOST_CHECK(loose_data->gateway_address.empty());
    const auto bare_data = shield::lua::ClientContextData::from_json(bare);
    BOOST_REQUIRE(bare_data.has_value());
    BOOST_CHECK_EQUAL(bare_data->session_id, 0u);
}

// ---------------------------------------------------------------------------
// HTTP + HTTPD + plugin introspection APIs.
// ---------------------------------------------------------------------------
#ifndef _WIN32  // MiniHttpServer uses raw POSIX sockets: not on Windows
BOOST_AUTO_TEST_CASE(HttpAndPluginApis) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    MiniHttpServer server;
    BOOST_REQUIRE_GE(server.port(), 1024);
    const std::string base =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/";
    lua["base_url"] = base;

    {
        std::error_code mk_ec;
        std::filesystem::create_directories("/tmp/opencode", mk_ec);
        std::ofstream up("/tmp/opencode/cov_upload.bin", std::ios::trunc);
        up << "upload-payload";
    }

    // Response table conversion incl. JSON auto-parse (header + heuristic)
    // and the invalid-JSON catch branch.
    BOOST_CHECK(run_script(
        lua,
        "local r = shield.http.get(base_url .. 'goodjson')\n"
        "assert(r.status == 200 and r.ok == true and r.error == '')\n"
        "local ct = r.headers['Content-Type'] or r.headers['content-type']\n"
        "assert(ct == 'application/json')\n"
        "assert(r.data.str == 's' and r.data.i == 1 and r.data.f == 1.5)\n"
        "assert(r.data.b == true and #r.data.arr == 2)\n"
        "assert(r.data.obj.k == 'v')"));
    BOOST_CHECK(run_script(lua,
                           "local r = shield.http.get(base_url .. 'badjson')\n"
                           "assert(r.data == nil and #r.body > 0)"));
    BOOST_CHECK(run_script(lua,
                           "local r = shield.http.get(base_url .. 'ctjson')\n"
                           "assert(r.data.ok == true)"));
    BOOST_CHECK(run_script(lua,
                           "local r = shield.http.get(base_url .. 'noct')\n"
                           "assert(r.data.x == 1)"));
    BOOST_CHECK(run_script(lua,
                           "local r = shield.http.get(base_url .. 'plain')\n"
                           "assert(r.body == 'plain' and r.data == nil)"));

    // Connection-refused error path (closed port, short timeout).
    BOOST_CHECK(run_script(
        lua,
        "local r = shield.http.get('http://127.0.0.1:9/x', {timeout = 2})\n"
        "assert(r.ok == false and #r.error > 0)"));

    // Full option parsing across every convenience wrapper.
    BOOST_CHECK(run_script(
        lua,
        "local opts = {\n"
        "  method = 'POST', body = '{}', timeout = 2,\n"
        "  headers = {h1 = 'v1', h2 = 42, [3] = 'skipped'},\n"
        "  auth_bearer = 'tok',\n"
        "  auth_basic = {user = 'u', password = 'p'},\n"
        "  proxy = '', verify_ssl = false,\n"
        "  ca_cert_path = '/tmp/opencode/cov_missing_ca.pem',\n"
        "  retry = 0, retry_delay = 10,\n"
        "  follow_redirects = false, max_redirects = 1,\n"
        "}\n"
        "local r = shield.http.request(base_url .. 'plain', opts)\n"
        "assert(r.status == 200)\n"
        "local p = shield.http.post(base_url .. 'plain', 'body', opts)\n"
        "assert(p.status == 200)\n"
        "local pu = shield.http.put(base_url .. 'plain', 'body', opts)\n"
        "assert(pu.status == 200)\n"
        "local d = shield.http.delete(base_url .. 'plain', opts)\n"
        "assert(d.status == 200)\n"
        "local pa = shield.http.patch(base_url .. 'plain', 'body', opts)\n"
        "assert(pa.status == 200)"));

    BOOST_CHECK(run_script(
        lua,
        "local opts = {timeout = 2}\n"
        "local j = shield.http.json(base_url .. 'plain', {a = 1, b = 'x'},"
        " opts)\n"
        "assert(j.status == 200)\n"
        "local jp = shield.http.json_post(base_url .. 'plain', {a = 1}, opts)\n"
        "assert(jp.status == 200)\n"
        "local jt = shield.http.json_put(base_url .. 'plain', {a = 1}, opts)\n"
        "assert(jt.status == 200)\n"
        "local jpa = shield.http.json_patch(base_url .. 'plain', {a = 1},"
        " opts)\n"
        "assert(jpa.status == 200)"));

    BOOST_CHECK(run_script(
        lua,
        "local files = {\n"
        "  {field_name = 'f', file_path = '/tmp/opencode/cov_upload.bin',\n"
        "   content_type = 'application/octet-stream'},\n"
        "  {not_a_real_entry = true},\n"
        "  5,\n"
        "}\n"
        "local fields = {k = 'v', n = 1, [2] = 'x'}\n"
        "local u = shield.http.upload(base_url .. 'plain', files, fields, 2)\n"
        "assert(u.status == 200)\n"
        "local dl = shield.http.download(base_url .. 'plain',\n"
        "  '/tmp/opencode/cov_download.out', 2)\n"
        "assert(dl.status == 200)\n"
        "local pf = shield.http.post_form(base_url .. 'plain',\n"
        "  {a = 'b', c = 'd', bad = 3}, 2)\n"
        "assert(pf.status == 200)"));

    // httpd route registration outside a service context must fail loudly
    // instead of silently reporting success.
    BOOST_CHECK(
        run_script(lua,
                   "local f = function() end\n"
                   "local ok, err = pcall(shield.httpd.get, '/a', f)\n"
                   "assert(ok == false)\n"
                   "assert(tostring(err):find('running service', 1, true))\n"
                   "local ok2, err2 = pcall(shield.httpd.post, '/b', f)\n"
                   "assert(ok2 == false)\n"
                   "assert(tostring(err2):find('running service', 1, true))"));

    // Plugin introspection backed by a manifest-only package.
    namespace fs = std::filesystem;
    const fs::path root = "/tmp/opencode/cov_plugin_pkgs";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "cov.pkg", ec);
    {
        std::ofstream m(root / "cov.pkg" / "manifest.yaml", std::ios::trunc);
        m << "schema_version: 1\n"
             "id: cov.pkg\n"
             "name: CovPkg\n"
             "version: 1.0.0\n"
             "kind: test\n"
             "entry: shield_plugin_get_v1\n"
             "library:\n"
             "  linux: bin/libcov.so\n"
             "  macos: bin/libcov.dylib\n"
             "  windows: bin/cov.dll\n"
             "provides:\n"
             "  - interface: cov.iface\n"
             "requires: []\n"
             "config_schema:\n"
             "  type: object\n";
    }
    auto& host = shield::plugin::global_host();
    host.scan(root.string());
    std::string perr;
    BOOST_REQUIRE(host.catalog(perr));
    shield::plugin::PluginConfig pcfg;
    shield::plugin::InstanceDecl decl;
    decl.id = "cov_inst";
    decl.package = "cov.pkg";
    decl.required = false;
    pcfg.instances.push_back(decl);
    shield::plugin::BindingDecl bind;
    bind.logical = "cov.binding";
    bind.instance_id = "cov_inst";
    pcfg.bindings.push_back(bind);
    BOOST_REQUIRE(host.plan_and_resolve(pcfg, perr));

    BOOST_CHECK(run_script(
        lua,
        "local pkgs = shield.plugin.packages()\n"
        "assert(#pkgs == 1 and pkgs[1].id == 'cov.pkg')\n"
        "assert(pkgs[1].version == '1.0.0' and pkgs[1].kind == 'test')\n"
        "assert(pkgs[1].provides[1] == 'cov.iface')\n"
        "local inst = shield.plugin.instances()\n"
        "assert(#inst == 1 and inst[1].id == 'cov_inst')\n"
        "assert(inst[1].package == 'cov.pkg')\n"
        "assert(type(inst[1].state) == 'string')\n"
        "local one = shield.plugin.instance('cov_inst')\n"
        "assert(one ~= nil and one.id == 'cov_inst')\n"
        "assert(shield.plugin.instance('missing') == nil)\n"
        "local b = shield.plugin.binding('cov.binding')\n"
        "assert(b ~= nil and b.instance_id == 'cov_inst')\n"
        "assert(b.interface == 'cov.iface')\n"
        "assert(shield.plugin.binding('missing') == nil)"));
}
#endif  // !_WIN32

BOOST_AUTO_TEST_SUITE_END()
