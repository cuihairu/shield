#define BOOST_TEST_MODULE NetworkConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/net/network_config.hpp"

using namespace shield::net;

BOOST_AUTO_TEST_SUITE(TcpConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    TcpConfig config;
    BOOST_CHECK(config.server.enabled);
    BOOST_CHECK_EQUAL(config.server.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.server.port, 8080);
    BOOST_CHECK_EQUAL(config.server.backlog, 128);
    BOOST_CHECK(config.server.keep_alive);
    BOOST_CHECK_EQUAL(config.server.max_connections, 1000);
    BOOST_CHECK_EQUAL(config.buffer.receive_buffer_size, 65536);
    BOOST_CHECK_EQUAL(config.buffer.send_buffer_size, 65536);
    BOOST_CHECK(config.buffer.no_delay);
    BOOST_CHECK(config.threading.use_thread_pool);
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    TcpConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "tcp");
}

BOOST_AUTO_TEST_CASE(TestIsEnabled) {
    TcpConfig config;
    BOOST_CHECK(config.is_enabled());
    config.server.enabled = false;
    BOOST_CHECK(!config.is_enabled());
}

BOOST_AUTO_TEST_CASE(TestValidateValid) {
    TcpConfig config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyHostThrows) {
    TcpConfig config;
    config.server.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroPortThrows) {
    TcpConfig config;
    config.server.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroBacklogThrows) {
    TcpConfig config;
    config.server.backlog = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroMaxConnectionsThrows) {
    TcpConfig config;
    config.server.max_connections = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroBufferThrows) {
    TcpConfig config;
    config.buffer.receive_buffer_size = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);

    TcpConfig config2;
    config2.buffer.send_buffer_size = 0;
    BOOST_CHECK_THROW(config2.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateDisabledSkipsServerChecks) {
    TcpConfig config;
    config.server.enabled = false;
    config.server.host = "";
    config.server.port = 0;
    // Should not throw because server is disabled
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestGetEffectiveIoThreads) {
    TcpConfig config;
    config.threading.io_threads = 4;
    BOOST_CHECK_EQUAL(config.get_effective_io_threads(), 4);

    config.threading.io_threads = 0;
    BOOST_CHECK_GT(config.get_effective_io_threads(), 0);
}

BOOST_AUTO_TEST_CASE(TestGetEffectiveWorkerThreads) {
    TcpConfig config;
    config.threading.worker_threads = 8;
    BOOST_CHECK_EQUAL(config.get_effective_worker_threads(), 8);

    config.threading.worker_threads = 0;
    BOOST_CHECK_GT(config.get_effective_worker_threads(), 0);
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    boost::property_tree::ptree pt;
    pt.put("server.enabled", true);
    pt.put("server.host", "192.168.1.100");
    pt.put("server.port", 9090);
    pt.put("server.backlog", 256);
    pt.put("server.max_connections", 500);
    pt.put("buffer.receive_buffer_size", 131072);
    pt.put("buffer.send_buffer_size", 131072);
    pt.put("buffer.no_delay", false);
    pt.put("threading.io_threads", 2);

    TcpConfig config;
    config.from_ptree(pt);

    BOOST_CHECK_EQUAL(config.server.host, "192.168.1.100");
    BOOST_CHECK_EQUAL(config.server.port, 9090);
    BOOST_CHECK_EQUAL(config.server.backlog, 256);
    BOOST_CHECK_EQUAL(config.server.max_connections, 500);
    BOOST_CHECK_EQUAL(config.buffer.receive_buffer_size, 131072);
    BOOST_CHECK_EQUAL(config.buffer.send_buffer_size, 131072);
    BOOST_CHECK(!config.buffer.no_delay);
    BOOST_CHECK_EQUAL(config.threading.io_threads, 2);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(UdpConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    UdpConfig config;
    BOOST_CHECK(config.server.enabled);
    BOOST_CHECK_EQUAL(config.server.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.server.port, 8081);
    BOOST_CHECK_EQUAL(config.server.buffer_size, 65536);
    BOOST_CHECK(config.performance.reuse_address);
    BOOST_CHECK(!config.performance.reuse_port);
    BOOST_CHECK_EQUAL(config.performance.receive_timeout, 5000);
    BOOST_CHECK_EQUAL(config.performance.send_timeout, 5000);
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    UdpConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "udp");
}

BOOST_AUTO_TEST_CASE(TestValidateValid) {
    UdpConfig config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyHostThrows) {
    UdpConfig config;
    config.server.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroPortThrows) {
    UdpConfig config;
    config.server.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroBufferThrows) {
    UdpConfig config;
    config.server.buffer_size = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateMaxPacketSizeZeroThrows) {
    UdpConfig config;
    config.server.max_packet_size = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateMaxPacketSizeTooLargeThrows) {
    UdpConfig config;
    config.server.max_packet_size = 65508;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateMaxPacketSizeBoundary) {
    UdpConfig config;
    config.server.max_packet_size = 65507;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateDisabledSkipsServerChecks) {
    UdpConfig config;
    config.server.enabled = false;
    config.server.host = "";
    config.server.port = 0;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestGetEffectiveIoThreads) {
    UdpConfig config;
    config.threading.io_threads = 3;
    BOOST_CHECK_EQUAL(config.get_effective_io_threads(), 3);

    config.threading.io_threads = 0;
    BOOST_CHECK_GT(config.get_effective_io_threads(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
