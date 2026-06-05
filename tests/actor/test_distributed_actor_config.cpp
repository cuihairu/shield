#define BOOST_TEST_MODULE DistributedActorConfigTest
#include <boost/test/unit_test.hpp>

#include "shield/actor/distributed_actor_system.hpp"

using namespace shield::actor;

BOOST_AUTO_TEST_SUITE(DistributedActorConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaults) {
    DistributedActorConfig config;
    BOOST_CHECK(config.node_id.empty());
    BOOST_CHECK_EQUAL(config.cluster_name, "shield_cluster");
    BOOST_CHECK_EQUAL(config.actor_port, 0);
    BOOST_CHECK_EQUAL(config.heartbeat_interval.count(), 30);
    BOOST_CHECK_EQUAL(config.discovery_interval.count(), 60);
    BOOST_CHECK(config.auto_discovery);
    BOOST_CHECK_EQUAL(config.max_remote_actors, 1000u);
}

BOOST_AUTO_TEST_CASE(TestCustomConfig) {
    DistributedActorConfig config;
    config.node_id = "node-1";
    config.cluster_name = "game_cluster";
    config.actor_port = 4000;
    config.heartbeat_interval = std::chrono::seconds{10};
    config.discovery_interval = std::chrono::seconds{30};
    config.auto_discovery = false;
    config.max_remote_actors = 500;

    BOOST_CHECK_EQUAL(config.node_id, "node-1");
    BOOST_CHECK_EQUAL(config.cluster_name, "game_cluster");
    BOOST_CHECK_EQUAL(config.actor_port, 4000);
    BOOST_CHECK_EQUAL(config.heartbeat_interval.count(), 10);
    BOOST_CHECK_EQUAL(config.discovery_interval.count(), 30);
    BOOST_CHECK(!config.auto_discovery);
    BOOST_CHECK_EQUAL(config.max_remote_actors, 500u);
}

BOOST_AUTO_TEST_CASE(TestCopy) {
    DistributedActorConfig config;
    config.node_id = "test";
    config.cluster_name = "cluster";

    DistributedActorConfig copy = config;
    BOOST_CHECK_EQUAL(copy.node_id, "test");
    BOOST_CHECK_EQUAL(copy.cluster_name, "cluster");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ActorSystemEventTests)

BOOST_AUTO_TEST_CASE(TestEventEnumDistinctness) {
    ActorSystemEvent events[] = {
        ActorSystemEvent::NODE_JOINED,
        ActorSystemEvent::NODE_LEFT,
        ActorSystemEvent::ACTOR_DISCOVERED,
        ActorSystemEvent::ACTOR_LOST,
        ActorSystemEvent::CLUSTER_CHANGED};

    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            BOOST_CHECK(events[i] != events[j]);
        }
    }
}

BOOST_AUTO_TEST_CASE(TestEventDataDefaults) {
    ActorSystemEventData data;
    BOOST_CHECK(data.node_id.empty());
    BOOST_CHECK(data.actor_name.empty());
    BOOST_CHECK(data.metadata.empty());
}

BOOST_AUTO_TEST_SUITE_END()
