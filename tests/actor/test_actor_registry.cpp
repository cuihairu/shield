#define BOOST_TEST_MODULE ActorRegistryTest
#include <boost/test/unit_test.hpp>

#include "shield/actor/actor_registry.hpp"

using namespace shield::actor;

BOOST_AUTO_TEST_SUITE(ActorMetadataTests)

BOOST_AUTO_TEST_CASE(TestTypeToString) {
    ActorMetadata meta;
    meta.type = ActorType::GATEWAY;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "gateway");

    meta.type = ActorType::LOGIC;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "logic");

    meta.type = ActorType::DATABASE;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "database");

    meta.type = ActorType::AUTH;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "auth");

    meta.type = ActorType::MONITOR;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "monitor");

    meta.type = ActorType::CUSTOM;
    BOOST_CHECK_EQUAL(meta.type_to_string(), "custom");
}

BOOST_AUTO_TEST_CASE(TestStringToType) {
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("gateway"),
                      ActorType::GATEWAY);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("logic"), ActorType::LOGIC);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("database"),
                      ActorType::DATABASE);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("auth"), ActorType::AUTH);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("monitor"),
                      ActorType::MONITOR);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("custom"),
                      ActorType::CUSTOM);
}

BOOST_AUTO_TEST_CASE(TestStringToTypeUnknown) {
    // Unknown type defaults to CUSTOM
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type("unknown"),
                      ActorType::CUSTOM);
    BOOST_CHECK_EQUAL(ActorMetadata::string_to_type(""), ActorType::CUSTOM);
}

BOOST_AUTO_TEST_CASE(TestTypeRoundTrip) {
    // type_to_string -> string_to_type should round-trip
    std::vector<ActorType> types = {
        ActorType::GATEWAY, ActorType::LOGIC,   ActorType::DATABASE,
        ActorType::AUTH,    ActorType::MONITOR, ActorType::CUSTOM};

    for (auto type : types) {
        ActorMetadata meta;
        meta.type = type;
        std::string str = meta.type_to_string();
        BOOST_CHECK_EQUAL(ActorMetadata::string_to_type(str), type);
    }
}

BOOST_AUTO_TEST_CASE(TestActorMetadataDefaults) {
    ActorMetadata meta;
    BOOST_CHECK_EQUAL(meta.load_weight, 100u);
    BOOST_CHECK(meta.name.empty());
    BOOST_CHECK(meta.node_id.empty());
    BOOST_CHECK(meta.service_group.empty());
    BOOST_CHECK(meta.tags.empty());
}

BOOST_AUTO_TEST_CASE(TestRegisteredActorDefaultConstructor) {
    RegisteredActor ra;
    BOOST_CHECK(!ra.actor_handle);
    BOOST_CHECK(ra.actor_uri.empty());
    BOOST_CHECK(!ra.is_local);
}

BOOST_AUTO_TEST_SUITE_END()
