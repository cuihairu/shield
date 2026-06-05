#define BOOST_TEST_MODULE ActorSystemCoordinatorTest
#include <boost/test/unit_test.hpp>

#include "shield/actor/actor_system_coordinator.hpp"

using namespace shield::actor;

BOOST_AUTO_TEST_SUITE(CoordinatorConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaults) {
    CoordinatorConfig config;
    BOOST_CHECK(config.node_id.empty());
    BOOST_CHECK_EQUAL(config.node_type, "logic");
    BOOST_CHECK_EQUAL(config.cluster_name, "shield_cluster");
    BOOST_CHECK_EQUAL(config.discovery_type, "in-memory");
    BOOST_CHECK(config.discovery_endpoints.empty());
    BOOST_CHECK_EQUAL(config.actor_port, 0);
    BOOST_CHECK_EQUAL(config.worker_threads, 4u);
    BOOST_CHECK(config.auto_start);
    BOOST_CHECK_EQUAL(config.heartbeat_interval.count(), 30);
    BOOST_CHECK_EQUAL(config.discovery_interval.count(), 60);
}

BOOST_AUTO_TEST_CASE(TestCustomConfig) {
    CoordinatorConfig config;
    config.node_id = "node-1";
    config.node_type = "gateway";
    config.cluster_name = "my_cluster";
    config.discovery_type = "consul";
    config.discovery_endpoints = "http://localhost:8500";
    config.actor_port = 4000;
    config.worker_threads = 8;
    config.auto_start = false;

    BOOST_CHECK_EQUAL(config.node_id, "node-1");
    BOOST_CHECK_EQUAL(config.node_type, "gateway");
    BOOST_CHECK_EQUAL(config.cluster_name, "my_cluster");
    BOOST_CHECK_EQUAL(config.discovery_type, "consul");
    BOOST_CHECK_EQUAL(config.actor_port, 4000);
    BOOST_CHECK_EQUAL(config.worker_threads, 8u);
    BOOST_CHECK(!config.auto_start);
}

BOOST_AUTO_TEST_SUITE_END()
