#define BOOST_TEST_MODULE DiscoveryServiceFactoryTest
#include <boost/test/unit_test.hpp>
#include <memory>

#include "shield/discovery/discovery_config.hpp"
#include "shield/discovery/discovery_service_factory.hpp"
#include "shield/discovery/service_discovery.hpp"

using namespace shield::discovery;

BOOST_AUTO_TEST_SUITE(DiscoveryServiceFactoryTests)

BOOST_AUTO_TEST_CASE(TestCreateWithNullConfig) {
    auto service = create_discovery_service(nullptr);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateLocalDiscovery) {
    DiscoveryConfig config;
    config.type = "local";
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateWithUnknownTypeFallsBackToLocal) {
    DiscoveryConfig config;
    config.type = "unknown_type";
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateRedisDiscovery) {
    DiscoveryConfig config;
    config.type = "redis";
    config.redis.host = "localhost";
    config.redis.port = 6379;
    // This will create a Redis discovery instance (may fail to connect
    // but should not crash on creation)
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateNacosDiscovery) {
    DiscoveryConfig config;
    config.type = "nacos";
    config.nacos.server_addr = "http://localhost:8848";
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateConsulDiscovery) {
    DiscoveryConfig config;
    config.type = "consul";
    config.consul.host = "localhost";
    config.consul.port = 8500;
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateEtcdDiscovery) {
    DiscoveryConfig config;
    config.type = "etcd";
    config.etcd.endpoints = {"http://localhost:2379"};
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestCreateEtcdWithMultipleEndpoints) {
    DiscoveryConfig config;
    config.type = "etcd";
    config.etcd.endpoints = {"http://host1:2379", "http://host2:2379",
                             "http://host3:2379"};
    auto service = create_discovery_service(&config);
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
