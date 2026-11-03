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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_library.hpp"
#include "shield/plugin/redis.h"

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
const char* g_current_instance = "incr_test_inst";
const char* resolver(shield_plugin_context_v1*, const char*) {
    return g_current_instance;
}

}  // namespace

#ifndef _WIN32
// The real-redis-server path (sw::redis fallback): overflow must surface
// cache_query_failed instead of a truncated value.
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

// ---------------------------------------------------------------------------
// Driver-path coverage (test gap): cache.redis must route incr_by through the
// redis.driver vtable when the host wires the dependency. The BUG-8 guard on
// the driver path (SHIELD_REDIS_INTEGER reply check -> cache_invalid_reply)
// was previously unreachable because the test never supplied a dependency.
// ---------------------------------------------------------------------------

namespace {

// Fake shield.redis.v2 driver that answers INCRBY with a configurable reply.
// The cache plugin calls connect() once during start(); free_value releases
// the reply node the command returned.
struct fake_driver {
    shield_redis_type_v1 reply_type = SHIELD_REDIS_INTEGER;
    int64_t reply_int = 0;
    int command_rc = 0;
    bool command_called = false;
    const shield_redis_v2* vtable = nullptr;
};

fake_driver* g_fake = nullptr;
const void* dependency_resolver(shield_plugin_context_v1*, const char* name,
                                const char* iface) {
    if (name && std::strcmp(name, "redis") == 0 && iface &&
        std::strcmp(iface, SHIELD_REDIS_V2) == 0 && g_fake) {
        return g_fake->vtable;
    }
    return nullptr;
}

}  // namespace

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(incr_by_driver_path_valid_integer) {
    fake_driver fake;
    fake.reply_type = SHIELD_REDIS_INTEGER;
    fake.reply_int = 15;
    g_fake = &fake;

    std::string err;
    PluginLibrary lib = PluginLibrary::load(SHIELD_CACHE_REDIS_LIBRARY, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);
    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    // Build a minimal fake vtable.
    static shield_redis_v2 vt{};
    vt.struct_size = sizeof(shield_redis_v2);
    vt.interface_name = SHIELD_REDIS_V2;
    vt.connect = [](const shield_redis_v2*, const void*, char*, int) {
        return static_cast<void*>(g_fake);
    };
    vt.disconnect = [](void*) {};
    vt.get = [](void*, const char*, shield_redis_value_v1**, shield_error_v1*) {
        return -1;
    };
    vt.set = [](void*, const char*, const char*, int, shield_error_v1*) {
        return 0;
    };
    vt.del = [](void*, const char*, shield_error_v1*) { return 0; };
    vt.hget = [](void*, const char*, const char*, shield_redis_value_v1**,
                 shield_error_v1*) { return -1; };
    vt.hset = [](void*, const char*, const char*, const char*,
                 shield_error_v1*) { return -1; };
    vt.hgetall = [](void*, const char*, shield_redis_value_v1**,
                    shield_error_v1*) { return -1; };
    vt.zadd = [](void*, const char*, double, const char*, shield_error_v1*) {
        return -1;
    };
    vt.zrange = [](void*, const char*, int, int, shield_redis_value_v1**,
                   shield_error_v1*) { return -1; };
    vt.pipeline = [](void*, const shield_redis_command_v1*, uint64_t,
                     shield_redis_value_v1***, uint64_t*,
                     shield_error_v1*) { return -1; };
    vt.command = [](void* handle, const shield_redis_arg_v1*, uint64_t,
                    shield_redis_value_v1** out, shield_error_v1* err) -> int {
        auto* d = static_cast<fake_driver*>(handle);
        if (!d) return -1;
        d->command_called = true;
        if (d->command_rc != 0) {
            if (err) {
                err->code = "redis.command.failed";
                err->message = "fake command failure";
            }
            return d->command_rc;
        }
        if (out) {
            *out = static_cast<shield_redis_value_v1*>(
                std::malloc(sizeof(shield_redis_value_v1)));
            if (!*out) return -1;
            std::memset(*out, 0, sizeof(shield_redis_value_v1));
            (*out)->type = d->reply_type;
            (*out)->integer = d->reply_int;
        }
        return 0;
    };
    vt.free_value = [](shield_redis_value_v1* v) { std::free(v); };
    fake.vtable = &vt;

    shield_host_api_v1 host_api{};
    host_api.binding_instance_id = resolver;
    host_api.dependency = dependency_resolver;
    shield_plugin_create_args_v1 args{};
    args.host_api = &host_api;
    args.instance_id = "incr_driver_test_inst";
    g_current_instance = "incr_driver_test_inst";
    args.config_json = "{}";
    shield_error_v1 cerr{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &cerr), 0);
    BOOST_REQUIRE(inst != nullptr);
    BOOST_REQUIRE_EQUAL(inst->start(inst, &cerr), 0);
    BOOST_REQUIRE(fake.command_called == false);

    shd::state lua;
    lua.open_libraries(shd::lib::base | shd::lib::string | shd::lib::table);
    shield_error_v1 rerr{};
    BOOST_REQUIRE_EQUAL(inst->register_lua(inst, lua.lua_state(), &rerr), 0);

    auto res = lua.script(R"lua(
        local c = shield.cache.redis("default")
        local ok, v = c.incr_by("k", 5)
        assert(ok == true and v == 15)
        return tostring(v)
    )lua");
    BOOST_REQUIRE(res.valid());
    BOOST_CHECK(fake.command_called);
    inst->shutdown(inst);
    g_fake = nullptr;
}

BOOST_AUTO_TEST_CASE(incr_by_driver_path_non_integer_reply) {
    fake_driver fake;
    fake.reply_type = SHIELD_REDIS_STRING;  // not an integer reply
    g_fake = &fake;

    std::string err;
    PluginLibrary lib = PluginLibrary::load(SHIELD_CACHE_REDIS_LIBRARY, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);
    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    static shield_redis_v2 vt{};
    vt.struct_size = sizeof(shield_redis_v2);
    vt.interface_name = SHIELD_REDIS_V2;
    vt.connect = [](const shield_redis_v2*, const void*, char*, int) {
        return static_cast<void*>(g_fake);
    };
    vt.disconnect = [](void*) {};
    vt.get = [](void*, const char*, shield_redis_value_v1**, shield_error_v1*) {
        return -1;
    };
    vt.set = [](void*, const char*, const char*, int, shield_error_v1*) {
        return 0;
    };
    vt.del = [](void*, const char*, shield_error_v1*) { return 0; };
    vt.hget = [](void*, const char*, const char*, shield_redis_value_v1**,
                 shield_error_v1*) { return -1; };
    vt.hset = [](void*, const char*, const char*, const char*,
                 shield_error_v1*) { return -1; };
    vt.hgetall = [](void*, const char*, shield_redis_value_v1**,
                    shield_error_v1*) { return -1; };
    vt.zadd = [](void*, const char*, double, const char*, shield_error_v1*) {
        return -1;
    };
    vt.zrange = [](void*, const char*, int, int, shield_redis_value_v1**,
                   shield_error_v1*) { return -1; };
    vt.pipeline = [](void*, const shield_redis_command_v1*, uint64_t,
                     shield_redis_value_v1***, uint64_t*,
                     shield_error_v1*) { return -1; };
    vt.command = [](void* handle, const shield_redis_arg_v1*, uint64_t,
                    shield_redis_value_v1** out, shield_error_v1* err) -> int {
        auto* d = static_cast<fake_driver*>(handle);
        if (!d) return -1;
        d->command_called = true;
        if (d->command_rc != 0) {
            if (err) {
                err->code = "redis.command.failed";
                err->message = "fake command failure";
            }
            return d->command_rc;
        }
        if (out) {
            *out = static_cast<shield_redis_value_v1*>(
                std::malloc(sizeof(shield_redis_value_v1)));
            if (!*out) return -1;
            std::memset(*out, 0, sizeof(shield_redis_value_v1));
            (*out)->type = d->reply_type;
            (*out)->integer = d->reply_int;
        }
        return 0;
    };
    vt.free_value = [](shield_redis_value_v1* v) { std::free(v); };
    fake.vtable = &vt;

    shield_host_api_v1 host_api{};
    host_api.binding_instance_id = resolver;
    host_api.dependency = dependency_resolver;
    shield_plugin_create_args_v1 args{};
    args.host_api = &host_api;
    args.instance_id = "incr_driver_test_inst2";
    g_current_instance = "incr_driver_test_inst2";
    args.config_json = "{}";
    shield_error_v1 cerr{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &cerr), 0);
    BOOST_REQUIRE(inst != nullptr);
    BOOST_REQUIRE_EQUAL(inst->start(inst, &cerr), 0);

    shd::state lua;
    lua.open_libraries(shd::lib::base | shd::lib::string | shd::lib::table);
    shield_error_v1 rerr{};
    BOOST_REQUIRE_EQUAL(inst->register_lua(inst, lua.lua_state(), &rerr), 0);

    // A non-integer INCRBY reply is invalid: the BUG-8 guard surfaces
    // cache_invalid_reply instead of truncating the value.
    auto res = lua.script(R"lua(
        local c = shield.cache.redis("default")
        local ok, err = c.incr_by("k", 5)
        assert(ok == false)
        assert(type(err) == 'table')
        assert(err.code == 'cache_invalid_reply')
        return err.code
    )lua");
    BOOST_REQUIRE(res.valid());
    BOOST_CHECK(fake.command_called);
    inst->shutdown(inst);
    g_fake = nullptr;
}
#endif
