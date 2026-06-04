#define BOOST_TEST_MODULE DiscoveryConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/discovery/discovery_config.hpp"

using namespace shield::discovery;

BOOST_AUTO_TEST_SUITE(DiscoveryConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    DiscoveryConfig config;
    BOOST_CHECK_EQUAL(config.type, "local");
    BOOST_CHECK_GT(config.local.cleanup_interval_seconds, 0);
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    DiscoveryConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "discovery");
}

BOOST_AUTO_TEST_CASE(TestValidateLocalValid) {
    DiscoveryConfig config;
    config.type = "local";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateInvalidTypeThrows) {
    DiscoveryConfig config;
    config.type = "invalid";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateEtcdEmptyEndpointsThrows) {
    DiscoveryConfig config;
    config.type = "etcd";
    config.etcd.endpoints.clear();
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateEtcdValid) {
    DiscoveryConfig config;
    config.type = "etcd";
    config.etcd.endpoints = {"http://localhost:2379"};
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateConsulEmptyHostThrows) {
    DiscoveryConfig config;
    config.type = "consul";
    config.consul.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateConsulZeroPortThrows) {
    DiscoveryConfig config;
    config.type = "consul";
    config.consul.host = "localhost";
    config.consul.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateConsulValid) {
    DiscoveryConfig config;
    config.type = "consul";
    config.consul.host = "localhost";
    config.consul.port = 8500;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateNacosEmptyAddrThrows) {
    DiscoveryConfig config;
    config.type = "nacos";
    config.nacos.server_addr = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateNacosValid) {
    DiscoveryConfig config;
    config.type = "nacos";
    config.nacos.server_addr = "http://localhost:8848";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateRedisEmptyHostThrows) {
    DiscoveryConfig config;
    config.type = "redis";
    config.redis.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateRedisZeroPortThrows) {
    DiscoveryConfig config;
    config.type = "redis";
    config.redis.host = "localhost";
    config.redis.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateRedisNegativeDbThrows) {
    DiscoveryConfig config;
    config.type = "redis";
    config.redis.host = "localhost";
    config.redis.port = 6379;
    config.redis.db = -1;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateRedisValid) {
    DiscoveryConfig config;
    config.type = "redis";
    config.redis.host = "localhost";
    config.redis.port = 6379;
    config.redis.db = 0;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestBuildRedisUriBasic) {
    DiscoveryConfig config;
    config.redis.host = "localhost";
    config.redis.port = 6379;
    config.redis.db = 0;
    BOOST_CHECK_EQUAL(config.build_redis_uri(), "tcp://localhost:6379");
}

BOOST_AUTO_TEST_CASE(TestBuildRedisUriWithPassword) {
    DiscoveryConfig config;
    config.redis.host = "localhost";
    config.redis.port = 6379;
    config.redis.password = "secret";
    config.redis.db = 0;
    BOOST_CHECK_EQUAL(config.build_redis_uri(),
                      "tcp://:secret@localhost:6379");
}

BOOST_AUTO_TEST_CASE(TestBuildRedisUriWithDb) {
    DiscoveryConfig config;
    config.redis.host = "localhost";
    config.redis.port = 6379;
    config.redis.db = 3;
    BOOST_CHECK_EQUAL(config.build_redis_uri(), "tcp://localhost:6379/3");
}

BOOST_AUTO_TEST_CASE(TestBuildRedisUriWithPasswordAndDb) {
    DiscoveryConfig config;
    config.redis.host = "redis.example.com";
    config.redis.port = 6380;
    config.redis.password = "pass";
    config.redis.db = 5;
    BOOST_CHECK_EQUAL(config.build_redis_uri(),
                      "tcp://:pass@redis.example.com:6380/5");
}

BOOST_AUTO_TEST_CASE(TestRedisHeartbeatInterval) {
    DiscoveryConfig config;
    config.redis.heartbeat_interval_seconds = 15;
    BOOST_CHECK_EQUAL(config.redis_heartbeat_interval().count(), 15);
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    boost::property_tree::ptree pt;
    pt.put("type", "consul");
    pt.put("consul.host", "consul.local");
    pt.put("consul.port", 8500);
    pt.put("consul.check_interval_seconds", 10);

    DiscoveryConfig config;
    config.from_ptree(pt);

    BOOST_CHECK_EQUAL(config.type, "consul");
    BOOST_CHECK_EQUAL(config.consul.host, "consul.local");
    BOOST_CHECK_EQUAL(config.consul.port, 8500);
    BOOST_CHECK_EQUAL(config.consul.check_interval_seconds, 10);
}

BOOST_AUTO_TEST_SUITE_END()
