// Plugin Lua facade contract: the callable namespaces plugins install via
// register_lua must fail SOFT on a missing/unresolvable binding —
//   local proxy, err = shield.<ns>("ghost.binding")
//   proxy == nil, err == { code = "module_unavailable", ... }
// (plugin-system.md usage rule 5). This pins the shape across every
// official facade plugin: no bare nil, no thrown error on a missing
// binding or on a no-arg call.
//
// The resolving half of the same contract (redis.driver): when the host
// resolver maps the logical binding to THIS instance, __call must return
// the per-instance proxy table with its full method surface. Like the
// soft-failure cases this needs no backend — the driver connects lazily on
// first method CALL, so asserting the method presence exercises the whole
// binding -> instance -> proxy resolution chain without a redis server.
//
// The test dlopens each plugin .so directly — no PluginHost pipeline and
// no backend connection: create() only builds the instance record and
// register_lua() only installs the namespace table, so the failure paths
// run without redis/mongodb servers. Each plugin case is enabled by its
// CMake build switch, so the test exercises whatever plugins the current
// build tree produces (the optional-plugins CI builds them all).

#define BOOST_TEST_MODULE shield_plugin_lua_facade

#include <boost/test/unit_test.hpp>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_library.hpp"

namespace {

using shield::plugin::PluginLibrary;

// Minimal host table: binding_instance_id always reports "absent" (NULL),
// which is the documented soft-failure input. Some plugins (mongodb) check
// the resolver's presence in register_lua, so host_api must be non-null —
// but no plugin touches anything else on this path.
const char* absent_binding(shield_plugin_context_v1*, const char*) {
    return nullptr;
}

// One plugin fixture: load the .so, create an instance wired to the
// caller's binding-resolver host, install the Lua namespace, then run
// `script` with its "NS(" placeholder rewritten to the real namespace name.
void facade_run(const char* library_path, const char* lua_namespace,
                const char* script,
                const char* (*resolver)(shield_plugin_context_v1*,
                                        const char*)) {
    std::string err;
    PluginLibrary lib = PluginLibrary::load(library_path, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);

    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);
    BOOST_CHECK_EQUAL(abi->abi_version, SHIELD_PLUGIN_ABI_VERSION);

    shield_host_api_v1 host_api{};
    host_api.binding_instance_id = resolver;
    shield_plugin_create_args_v1 args{};
    args.host_api = &host_api;
    args.instance_id = "facade_test";
    args.config_json = "{}";
    shield_error_v1 cerr{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &cerr), 0);
    BOOST_REQUIRE(inst != nullptr);

    shd::state lua;
    lua.open_libraries(shd::lib::base | shd::lib::string | shd::lib::table);
    shield_error_v1 rerr{};
    BOOST_CHECK_EQUAL(inst->register_lua(inst, lua.lua_state(), &rerr), 0);

    std::string expanded = script;
    const std::string placeholder = "NS(";
    const std::string real = std::string(lua_namespace) + "(";
    for (std::size_t pos = expanded.find(placeholder); pos != std::string::npos;
         pos = expanded.find(placeholder)) {
        expanded.replace(pos, placeholder.size(), real);
    }
    lua.script(expanded);

    inst->shutdown(inst);
}

// The documented soft-failure input: resolver always reports "absent".
void facade_soft_failure(const char* library_path, const char* lua_namespace,
                         const char* script) {
    facade_run(library_path, lua_namespace, script, &absent_binding);
}

// Shared Lua assertions: the namespace fails soft for a missing binding
// and for a no-arg call, and reports the binding back in the error table.
const char* kSoftFailureScript = R"(
    local proxy, err = NS("ghost.binding")
    assert(proxy == nil, "expected nil proxy")
    assert(type(err) == "table", "expected error table")
    assert(err.code == "module_unavailable", "wrong code: " .. tostring(err.code))
    assert(err.binding == "ghost.binding", "binding not echoed")

    local p2, err2 = NS()
    assert(p2 == nil, "no-arg call must fail soft, not throw")
    assert(err2.code == "module_unavailable")
)";

// Resolving host for the redis.driver case: "redis-main" maps to the
// instance the fixture created ("facade_test"); everything else stays
// absent so the same script also re-checks the soft-failure arm through
// the resolving namespace.
const char* resolving_binding(shield_plugin_context_v1*, const char* binding) {
    if (binding && std::string(binding) == "redis-main") return "facade_test";
    return nullptr;
}

// Same shape for the metrics.prometheus case.
const char* resolving_metric_binding(shield_plugin_context_v1*,
                                     const char* binding) {
    if (binding && std::string(binding) == "metrics-main") return "facade_test";
    return nullptr;
}

const char* kResolvingScript = R"(
    local proxy, err = NS("redis-main")
    assert(proxy ~= nil, "expected proxy, got error: " .. tostring(err))
    assert(type(proxy) == "table", "proxy is " .. type(proxy))

    -- The driver's documented proxy surface (redis-driver docs): the ten
    -- typed command methods. Presence only — calling would need a server.
    local methods = { "get", "set", "del", "hget", "hset", "hgetall",
                      "zadd", "zrange", "command", "pipeline" }
    for _, m in ipairs(methods) do
        assert(type(proxy[m]) == "function",
               "proxy." .. m .. " is " .. type(proxy[m]))
    end

    -- The same namespace still fails soft for an unresolvable binding.
    local p2, err2 = NS("ghost.binding")
    assert(p2 == nil, "unresolvable binding must fail soft")
    assert(err2.code == "module_unavailable", "wrong code: " ..
           tostring(err2 and err2.code))
)";

// The metrics.prometheus resolving arm: recording lands in the plugin's
// in-memory registry, so the typed methods run for real — no server and no
// port needed. Pins the colon/dot call-shape equivalence and the
// invalid-labels error table alongside the soft-failure arm.
const char* kMetricResolvingScript = R"(
    local proxy, err = NS("metrics-main")
    assert(proxy ~= nil, "expected proxy, got error: " .. tostring(err))
    assert(type(proxy) == "table", "proxy is " .. type(proxy))

    -- The documented proxy surface: the three typed record methods.
    local methods = { "counter", "gauge", "histogram" }
    for _, m in ipairs(methods) do
        assert(type(proxy[m]) == "function",
               "proxy." .. m .. " is " .. type(proxy[m]))
    end

    -- Recording works through both call shapes.
    assert(proxy:counter("lua_test_total", 1) == true)
    assert(proxy.counter("lua_test_total", 2) == true)
    assert(proxy:gauge("lua_test_depth", 9) == true)
    assert(proxy:histogram("lua_test_ms", 1.5) == true)
    assert(proxy:counter("lua_labeled_total", 1, { route = "echo" }) == true)

    -- Non-string label keys/values are rejected with false + error table.
    local ok, lerr = proxy:counter("lua_bad_total", 1, { 42 })
    assert(ok == false, "non-string label must fail, not error")
    assert(type(lerr) == "table", "expected error table")
    assert(lerr.code == "invalid_labels", "wrong code: " ..
           tostring(lerr and lerr.code))

    -- The same namespace still fails soft for an unresolvable binding.
    local p2, err2 = NS("ghost.binding")
    assert(p2 == nil, "unresolvable binding must fail soft")
    assert(err2.code == "module_unavailable")
)";

}  // namespace

#ifdef SHIELD_FACADE_SQLITE_LIBRARY
BOOST_AUTO_TEST_CASE(DatabaseSqliteFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_SQLITE_LIBRARY, "shield.database.sqlite",
                        kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_MONGODB_LIBRARY
BOOST_AUTO_TEST_CASE(DatabaseMongodbFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_MONGODB_LIBRARY,
                        "shield.database.mongodb", kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_MYSQL_LIBRARY
BOOST_AUTO_TEST_CASE(DatabaseMysqlFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_MYSQL_LIBRARY, "shield.database.mysql",
                        kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_POSTGRESQL_LIBRARY
BOOST_AUTO_TEST_CASE(DatabasePostgresqlFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_POSTGRESQL_LIBRARY,
                        "shield.database.postgresql", kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_CACHE_REDIS_LIBRARY
BOOST_AUTO_TEST_CASE(CacheRedisFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_CACHE_REDIS_LIBRARY, "shield.cache.redis",
                        kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_QUEUE_REDIS_LIBRARY
BOOST_AUTO_TEST_CASE(QueueRedisFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_QUEUE_REDIS_LIBRARY, "shield.queue.redis",
                        kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_LEADERBOARD_REDIS_LIBRARY
BOOST_AUTO_TEST_CASE(LeaderboardRedisFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_LEADERBOARD_REDIS_LIBRARY,
                        "shield.leaderboard.redis", kSoftFailureScript);
}
#endif

#ifdef SHIELD_FACADE_REDIS_DRIVER_LIBRARY
BOOST_AUTO_TEST_CASE(RedisDriverFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_REDIS_DRIVER_LIBRARY, "shield.redis",
                        kSoftFailureScript);
}

// The resolving arm: host maps "redis-main" -> the created instance, so
// __call builds and returns the real per-instance proxy (no server needed —
// method presence only).
BOOST_AUTO_TEST_CASE(RedisDriverFacadeResolvesBinding) {
    facade_run(SHIELD_FACADE_REDIS_DRIVER_LIBRARY, "shield.redis",
               kResolvingScript, &resolving_binding);
}
#endif

#ifdef SHIELD_FACADE_METRIC_LIBRARY
BOOST_AUTO_TEST_CASE(MetricPrometheusFacadeSoftFailure) {
    facade_soft_failure(SHIELD_FACADE_METRIC_LIBRARY, "shield.metrics",
                        kSoftFailureScript);
}

// The resolving arm runs the typed record methods for real (in-memory
// registry): colon/dot shape equivalence and the invalid-labels error
// table are pinned here, not just method presence.
BOOST_AUTO_TEST_CASE(MetricPrometheusFacadeResolvesBinding) {
    facade_run(SHIELD_FACADE_METRIC_LIBRARY, "shield.metrics",
               kMetricResolvingScript, &resolving_metric_binding);
}
#endif
