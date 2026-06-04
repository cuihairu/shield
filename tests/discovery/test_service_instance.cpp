#define BOOST_TEST_MODULE ServiceInstanceTest
#include <boost/test/unit_test.hpp>
#include <map>
#include <string>
#include <vector>

#include "shield/discovery/service_instance.hpp"

using namespace shield::discovery;

BOOST_AUTO_TEST_SUITE(ServiceMetadataTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    ServiceMetadata meta;
    BOOST_CHECK(meta.version.empty());
    BOOST_CHECK(meta.region.empty());
    BOOST_CHECK(meta.environment.empty());
    BOOST_CHECK_EQUAL(meta.weight, 0u);
    BOOST_CHECK(meta.tags.empty());
    BOOST_CHECK(meta.custom_attributes.empty());
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersVersion) {
    ServiceMetadata meta;
    meta.version = "1.0.0";

    std::map<std::string, std::string> filters = {{"version", "1.0.0"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["version"] = "2.0.0";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersRegion) {
    ServiceMetadata meta;
    meta.region = "us-east";

    std::map<std::string, std::string> filters = {{"region", "us-east"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["region"] = "eu-west";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersEnvironment) {
    ServiceMetadata meta;
    meta.environment = "production";

    std::map<std::string, std::string> filters = {{"environment", "production"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["environment"] = "staging";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersWeight) {
    ServiceMetadata meta;
    meta.weight = 100;

    std::map<std::string, std::string> filters = {{"weight", "100"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["weight"] = "200";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersTag) {
    ServiceMetadata meta;
    meta.tags = {"primary", "v2"};

    std::map<std::string, std::string> filters = {{"tag", "primary"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["tag"] = "missing";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersCustomAttributes) {
    ServiceMetadata meta;
    meta.custom_attributes = {{"zone", "a"}, {"rack", "1"}};

    std::map<std::string, std::string> filters = {{"zone", "a"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["zone"] = "b";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesFiltersUnknownKeyReturnsFalse) {
    ServiceMetadata meta;
    std::map<std::string, std::string> filters = {{"unknown_key", "value"}};
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesEmptyFilters) {
    ServiceMetadata meta;
    meta.version = "1.0.0";
    std::map<std::string, std::string> filters;
    BOOST_CHECK(meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestMatchesMultipleFilters) {
    ServiceMetadata meta;
    meta.version = "1.0.0";
    meta.region = "us-east";
    meta.tags = {"primary"};

    std::map<std::string, std::string> filters = {
        {"version", "1.0.0"}, {"region", "us-east"}, {"tag", "primary"}};
    BOOST_CHECK(meta.matches_filters(filters));

    filters["region"] = "eu-west";
    BOOST_CHECK(!meta.matches_filters(filters));
}

BOOST_AUTO_TEST_CASE(TestJsonRoundTrip) {
    ServiceMetadata meta;
    meta.version = "2.0.0";
    meta.region = "eu-west";
    meta.environment = "staging";
    meta.weight = 50;
    meta.tags = {"tag1", "tag2"};
    meta.custom_attributes = {{"key1", "val1"}};

    std::string json = meta.to_json();
    BOOST_CHECK(!json.empty());

    auto restored = ServiceMetadata::from_json(json);
    BOOST_CHECK_EQUAL(restored.version, "2.0.0");
    BOOST_CHECK_EQUAL(restored.region, "eu-west");
    BOOST_CHECK_EQUAL(restored.environment, "staging");
    BOOST_CHECK_EQUAL(restored.weight, 50u);
    BOOST_REQUIRE_EQUAL(restored.tags.size(), 2);
    BOOST_CHECK_EQUAL(restored.tags[0], "tag1");
    BOOST_CHECK_EQUAL(restored.tags[1], "tag2");
    BOOST_CHECK_EQUAL(restored.custom_attributes.at("key1"), "val1");
}

BOOST_AUTO_TEST_CASE(TestFromJsonEmpty) {
    auto meta = ServiceMetadata::from_json("");
    BOOST_CHECK(meta.version.empty());
}

BOOST_AUTO_TEST_CASE(TestFromJsonInvalid) {
    auto meta = ServiceMetadata::from_json("not json");
    BOOST_CHECK(meta.version.empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ServiceInstanceJsonTests)

BOOST_AUTO_TEST_CASE(TestServiceInstanceJsonRoundTrip) {
    ServiceInstance inst;
    inst.service_name = "auth-service";
    inst.instance_id = "inst-1";
    inst.address = "tcp://127.0.0.1:9001";
    inst.metadata.version = "1.0.0";
    inst.metadata.region = "us-east";
    inst.expiration_time = std::chrono::steady_clock::time_point::max();

    nlohmann::json j;
    to_json(j, inst);

    BOOST_CHECK_EQUAL(j["service_name"], "auth-service");
    BOOST_CHECK_EQUAL(j["instance_id"], "inst-1");
    BOOST_CHECK_EQUAL(j["address"], "tcp://127.0.0.1:9001");

    ServiceInstance restored;
    from_json(j, restored);

    BOOST_CHECK_EQUAL(restored.service_name, "auth-service");
    BOOST_CHECK_EQUAL(restored.instance_id, "inst-1");
    BOOST_CHECK_EQUAL(restored.address, "tcp://127.0.0.1:9001");
    BOOST_CHECK_EQUAL(restored.metadata.version, "1.0.0");
    BOOST_CHECK_EQUAL(restored.metadata.region, "us-east");
}

BOOST_AUTO_TEST_SUITE_END()
