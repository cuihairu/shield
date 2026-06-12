#define BOOST_TEST_MODULE BeastHttpServerConfigTest
#include <boost/test/unit_test.hpp>

#include "shield/net/http/beast_http_server.hpp"

using namespace shield::http;

BOOST_AUTO_TEST_SUITE(BeastHttpServerConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaults) {
    BeastHttpServerConfig config;
    BOOST_CHECK_EQUAL(config.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.port, 8082);
    BOOST_CHECK_EQUAL(config.threads, 0);
    BOOST_CHECK_EQUAL(config.root_path, "/");
    BOOST_CHECK_EQUAL(config.max_request_size, 1024u * 1024u);
}

BOOST_AUTO_TEST_CASE(TestCustomConfig) {
    BeastHttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 9090;
    config.threads = 4;
    config.root_path = "/var/www";
    config.max_request_size = 2 * 1024 * 1024;

    BOOST_CHECK_EQUAL(config.host, "127.0.0.1");
    BOOST_CHECK_EQUAL(config.port, 9090);
    BOOST_CHECK_EQUAL(config.threads, 4);
    BOOST_CHECK_EQUAL(config.root_path, "/var/www");
    BOOST_CHECK_EQUAL(config.max_request_size, 2u * 1024u * 1024u);
}

BOOST_AUTO_TEST_CASE(TestCopy) {
    BeastHttpServerConfig config;
    config.host = "10.0.0.1";
    config.port = 3000;

    BeastHttpServerConfig copy = config;
    BOOST_CHECK_EQUAL(copy.host, "10.0.0.1");
    BOOST_CHECK_EQUAL(copy.port, 3000);
}

BOOST_AUTO_TEST_SUITE_END()
