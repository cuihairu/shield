#define BOOST_TEST_MODULE GatewayConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/gateway/gateway_config.hpp"

using namespace shield::gateway;

BOOST_AUTO_TEST_SUITE(GatewayConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    GatewayConfig config;
    BOOST_CHECK_EQUAL(config.listener.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.listener.port, 8080);
    BOOST_CHECK(config.tcp.enabled);
    BOOST_CHECK_EQUAL(config.tcp.backlog, 128);
    BOOST_CHECK(config.udp.enabled);
    BOOST_CHECK_EQUAL(config.udp.buffer_size, 65536);
    BOOST_CHECK(!config.http.enabled);
    BOOST_CHECK(config.websocket.enabled);
    BOOST_CHECK_EQUAL(config.websocket.ping_interval, 30);
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    GatewayConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "gateway");
}

BOOST_AUTO_TEST_CASE(TestValidateValidConfig) {
    GatewayConfig config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyHostThrows) {
    GatewayConfig config;
    config.listener.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroPortThrows) {
    GatewayConfig config;
    config.listener.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateTcpBacklogThrows) {
    GatewayConfig config;
    config.tcp.enabled = true;
    config.tcp.backlog = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateUdpBufferSizeThrows) {
    GatewayConfig config;
    config.udp.enabled = true;
    config.udp.buffer_size = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateHttpPortConflictThrows) {
    GatewayConfig config;
    config.http.enabled = true;
    config.http.port = 8080;  // same as listener.port
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateHttpInvalidBackendThrows) {
    GatewayConfig config;
    config.http.enabled = true;
    config.http.port = 9090;
    config.http.backend = "invalid";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateHttpValidBackend) {
    GatewayConfig config;
    config.http.enabled = true;
    config.http.port = 9090;
    config.http.backend = "beast";
    BOOST_CHECK_NO_THROW(config.validate());

    config.http.backend = "legacy";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateUdpPortConflictThrows) {
    GatewayConfig config;
    config.udp.enabled = true;
    config.udp.port = 8080;  // same as listener.port
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateWsPortConflictThrows) {
    GatewayConfig config;
    config.websocket.enabled = true;
    config.websocket.port = 8080;  // same as listener.port
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateWsPingIntervalThrows) {
    GatewayConfig config;
    config.websocket.enabled = true;
    config.websocket.ping_interval = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateHttpWsPortConflictThrows) {
    GatewayConfig config;
    config.http.enabled = true;
    config.http.port = 9090;
    config.websocket.enabled = true;
    config.websocket.port = 9090;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestGetEffectiveIoThreads) {
    GatewayConfig config;
    config.threading.io_threads = 4;
    BOOST_CHECK_EQUAL(config.get_effective_io_threads(), 4);

    config.threading.io_threads = 0;
    config.listener.io_threads = 2;
    BOOST_CHECK_EQUAL(config.get_effective_io_threads(), 2);

    config.listener.io_threads = 0;
    // Should fall back to hardware_concurrency
    BOOST_CHECK_GT(config.get_effective_io_threads(), 0);
}

BOOST_AUTO_TEST_CASE(TestGetEffectiveWorkerThreads) {
    GatewayConfig config;
    config.threading.worker_threads = 8;
    BOOST_CHECK_EQUAL(config.get_effective_worker_threads(), 8);

    config.threading.worker_threads = 0;
    BOOST_CHECK_GT(config.get_effective_worker_threads(), 0);
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    boost::property_tree::ptree pt;
    pt.put("listener.host", "192.168.1.1");
    pt.put("listener.port", 9090);
    pt.put("tcp.enabled", false);
    pt.put("tcp.backlog", 256);
    pt.put("udp.enabled", false);
    pt.put("http.enabled", true);
    pt.put("http.port", 8088);
    pt.put("http.backend", "beast");
    pt.put("websocket.enabled", false);

    GatewayConfig config;
    config.from_ptree(pt);

    BOOST_CHECK_EQUAL(config.listener.host, "192.168.1.1");
    BOOST_CHECK_EQUAL(config.listener.port, 9090);
    BOOST_CHECK(!config.tcp.enabled);
    BOOST_CHECK_EQUAL(config.tcp.backlog, 256);
    BOOST_CHECK(!config.udp.enabled);
    BOOST_CHECK(config.http.enabled);
    BOOST_CHECK_EQUAL(config.http.port, 8088);
    BOOST_CHECK_EQUAL(config.http.backend, "beast");
    BOOST_CHECK(!config.websocket.enabled);
}

BOOST_AUTO_TEST_SUITE_END()
