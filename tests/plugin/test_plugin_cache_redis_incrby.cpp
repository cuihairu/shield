// BUG-8 regression: cache.redis incr_by must not silently return a wrapped
// value when Redis reports an overflow. The INCRBY result (or the argument)
// beyond a signed 64-bit range is an error at the Redis server; sw::redis
// throws and the proxy must surface {false, {code="cache_query_failed", ...}}
// instead of a truncated/undefined value.
//
// Runs against a real redis-server (skipped when unavailable) through the
// plugin ABI + register_lua, exactly like the facade test but with a
// binding resolver that maps the logical name to this instance.
#define BOOST_TEST_MODULE CacheRedisIncrBy
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_library.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using shield::plugin::PluginLibrary;

#ifndef _WIN32
struct redis_server {
    int port = 0;
    ::pid_t pid = -1;
    ~redis_server() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

int probe_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_accepts(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

std::unique_ptr<redis_server> spawn_redis_server() {
    const int port = probe_free_port();
    if (port <= 0) return nullptr;
    const ::pid_t pid = ::fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        const auto port_str = std::to_string(port);
        ::execlp("redis-server", "redis-server", "--port", port_str.c_str(),
                 "--save", "", "--appendonly", "no", nullptr);
        ::_exit(127);
    }
    for (int i = 0; i < 100; ++i) {
        if (tcp_accepts(port)) {
            auto srv = std::make_unique<redis_server>();
            srv->port = port;
            srv->pid = pid;
            return srv;
        }
        ::usleep(50000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return nullptr;
}
#endif

// Resolver mapping the logical binding to the created instance id.
const char* resolver(shield_plugin_context_v1*, const char*) {
    return "incr_test_inst";
}

}  // namespace

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(incr_by_overflow_reports_error) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }

    std::string err;
    PluginLibrary lib = PluginLibrary::load(SHIELD_CACHE_REDIS_LIBRARY, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);

    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    shield_host_api_v1 host_api{};
    host_api.binding_instance_id = resolver;
    shield_plugin_create_args_v1 args{};
    args.host_api = &host_api;
    args.instance_id = "incr_test_inst";
    const std::string config_json =
        R"({"host":"127.0.0.1","port":)" + std::to_string(srv->port) + "}";
    args.config_json = config_json.c_str();
    shield_error_v1 cerr{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &cerr), 0);
    BOOST_REQUIRE(inst != nullptr);

    shd::state lua;
    lua.open_libraries(shd::lib::base | shd::lib::string | shd::lib::table);
    shield_error_v1 rerr{};
    BOOST_REQUIRE_EQUAL(inst->register_lua(inst, lua.lua_state(), &rerr), 0);

    // Baseline: a normal incr_by returns the new value.
    {
        auto res = lua.script(R"lua(
            local c = shield.cache.redis("default")
            c.set("k", "10")
            local ok, v = c.incr_by("k", 5)
            assert(ok == true and v == 15)
            return tostring(v)
        )lua");
        BOOST_REQUIRE(res.valid());
    }

    // Overflow: set near LLONG_MAX then add a positive amount. sw::redis
    // throws, the proxy must return false with a stable error code.
    {
        auto res = lua.script(R"lua(
            local c = shield.cache.redis("default")
            c.set("k", "9223372036854775806")
            local ok, err = c.incr_by("k", 10)
            assert(ok == false)
            assert(type(err) == 'table')
            assert(err.code == 'cache_query_failed')
            return err.code
        )lua");
        BOOST_REQUIRE(res.valid());
    }

    inst->shutdown(inst);
}
#endif
