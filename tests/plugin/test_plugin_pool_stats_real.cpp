// Real-plugin shield.pool.stats.v1 contract: every plugin whose manifest
// declares the interface must serve a structurally valid vtable from
// get_interface() and answer get_stats() with 0 — without any backend
// connection (create() only builds the instance record, same as the Lua
// facade test). Field values are plugin-defined: a driver-backed pool
// reports real gauges (max_size >= 1); a plugin without a persistent pool
// reports -1 (unknown). This test pins the ABI contract, not the values.
//
// Each plugin case is enabled by its CMake build switch, so the test
// exercises whatever plugins the current build tree produces (the
// optional-plugins CI builds mysql/postgresql; sqlite deliberately does
// not declare the interface).

#define BOOST_TEST_MODULE shield_plugin_pool_stats_real

#include <boost/test/unit_test.hpp>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_library.hpp"
#include "shield/plugin/pool_stats.h"

namespace {

using shield::plugin::PluginLibrary;

const char* absent_binding(shield_plugin_context_v1*, const char*) {
    return nullptr;
}

void pool_stats_contract(const char* library_path) {
    std::string err;
    PluginLibrary lib = PluginLibrary::load(library_path, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);

    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    shield_host_api_v1 host_api{};
    host_api.binding_instance_id = &absent_binding;
    shield_plugin_create_args_v1 args{};
    args.host_api = &host_api;
    args.instance_id = "pool_stats_test";
    args.config_json = "{}";
    shield_error_v1 cerr{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &cerr), 0);
    BOOST_REQUIRE(inst != nullptr);

    // Declared in manifest.yaml ⇒ get_interface must serve the vtable.
    shield_error_v1 ierr{};
    const void* raw =
        inst->get_interface(inst, SHIELD_POOL_STATS_INTERFACE, &ierr);
    BOOST_REQUIRE_MESSAGE(raw != nullptr, library_path
                                              << " does not serve "
                                              << SHIELD_POOL_STATS_INTERFACE);

    const auto* vt = static_cast<const shield_pool_stats_v1*>(raw);
    BOOST_CHECK_GE(vt->struct_size, sizeof(shield_pool_stats_v1));
    BOOST_REQUIRE(vt->get_stats != nullptr);

    shield_pool_stats stats{};
    stats.struct_size = sizeof(shield_pool_stats);  // host sentinel contract
    const int rc = vt->get_stats(inst, &stats);
    BOOST_CHECK_EQUAL(rc, 0);
    // Real gauges (>= 1) or explicit unknown (-1); never a bogus 0 cap.
    BOOST_CHECK_MESSAGE(stats.max_size >= 1 || stats.max_size == -1,
                        "max_size out of contract: " << stats.max_size);
    BOOST_CHECK_MESSAGE(stats.size == -1 || stats.size >= 0,
                        "size out of contract: " << stats.size);

    inst->shutdown(inst);
}

}  // namespace

#ifdef SHIELD_FACADE_MYSQL_LIBRARY
BOOST_AUTO_TEST_CASE(MySqlPoolStatsContract) {
    pool_stats_contract(SHIELD_FACADE_MYSQL_LIBRARY);
}
#endif

#ifdef SHIELD_FACADE_POSTGRESQL_LIBRARY
BOOST_AUTO_TEST_CASE(PostgreSqlPoolStatsContract) {
    pool_stats_contract(SHIELD_FACADE_POSTGRESQL_LIBRARY);
}
#endif

#ifdef SHIELD_FACADE_MONGODB_LIBRARY
BOOST_AUTO_TEST_CASE(MongoDbPoolStatsContract) {
    pool_stats_contract(SHIELD_FACADE_MONGODB_LIBRARY);
}
#endif

#ifdef SHIELD_FACADE_QUEUE_REDIS_LIBRARY
BOOST_AUTO_TEST_CASE(QueueRedisPoolStatsContract) {
    pool_stats_contract(SHIELD_FACADE_QUEUE_REDIS_LIBRARY);
}
#endif

#ifdef SHIELD_FACADE_LEADERBOARD_REDIS_LIBRARY
BOOST_AUTO_TEST_CASE(LeaderboardRedisPoolStatsContract) {
    pool_stats_contract(SHIELD_FACADE_LEADERBOARD_REDIS_LIBRARY);
}
#endif
