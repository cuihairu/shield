#define BOOST_TEST_MODULE PrometheusConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/metrics/prometheus_config.hpp"

using namespace shield::metrics;

BOOST_AUTO_TEST_SUITE(PrometheusConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    PrometheusConfig config;
    BOOST_CHECK(config.server.enabled);
    BOOST_CHECK_EQUAL(config.server.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.server.port, 9090);
    BOOST_CHECK_EQUAL(config.server.path, "/metrics");
    BOOST_CHECK_EQUAL(config.server.max_connections, 100);
    BOOST_CHECK(config.system_metrics.enabled);
    BOOST_CHECK_EQUAL(config.system_metrics.collection_interval, 5);
    BOOST_CHECK(config.system_metrics.collect_cpu);
    BOOST_CHECK(config.system_metrics.collect_memory);
    BOOST_CHECK(config.app_metrics.enabled);
    BOOST_CHECK(config.app_metrics.collect_http_requests);
    BOOST_CHECK_EQUAL(config.export_config.format, "prometheus");
    BOOST_CHECK_EQUAL(config.export_config.namespace_prefix, "shield");
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    PrometheusConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "prometheus");
}

BOOST_AUTO_TEST_CASE(TestIsMetricsEnabled) {
    PrometheusConfig config;
    BOOST_CHECK(config.is_metrics_enabled());
    config.server.enabled = false;
    BOOST_CHECK(!config.is_metrics_enabled());
}

BOOST_AUTO_TEST_CASE(TestGetMetricsEndpoint) {
    PrometheusConfig config;
    config.server.host = "localhost";
    config.server.port = 9090;
    config.server.path = "/metrics";
    BOOST_CHECK_EQUAL(config.get_metrics_endpoint(),
                      "http://localhost:9090/metrics");
}

BOOST_AUTO_TEST_CASE(TestGetMetricsEndpointCustomPath) {
    PrometheusConfig config;
    config.server.host = "0.0.0.0";
    config.server.port = 8080;
    config.server.path = "/prometheus";
    BOOST_CHECK_EQUAL(config.get_metrics_endpoint(),
                      "http://0.0.0.0:8080/prometheus");
}

BOOST_AUTO_TEST_CASE(TestValidateValid) {
    PrometheusConfig config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyHost) {
    PrometheusConfig config;
    config.server.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroPort) {
    PrometheusConfig config;
    config.server.port = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyPath) {
    PrometheusConfig config;
    config.server.path = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidatePathWithoutSlash) {
    PrometheusConfig config;
    config.server.path = "metrics";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidatePathWithSlash) {
    PrometheusConfig config;
    config.server.path = "/metrics";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateZeroMaxConnections) {
    PrometheusConfig config;
    config.server.max_connections = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateDisabledServerSkipsChecks) {
    PrometheusConfig config;
    config.server.enabled = false;
    config.server.host = "";
    config.server.port = 0;
    config.server.path = "";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateZeroCollectionInterval) {
    PrometheusConfig config;
    config.system_metrics.enabled = true;
    config.system_metrics.collection_interval = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateInvalidExportFormat) {
    PrometheusConfig config;
    config.export_config.format = "csv";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateValidExportFormats) {
    PrometheusConfig config;
    config.export_config.format = "prometheus";
    BOOST_CHECK_NO_THROW(config.validate());
    config.export_config.format = "json";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyNamespacePrefix) {
    PrometheusConfig config;
    config.export_config.namespace_prefix = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    boost::property_tree::ptree pt;
    pt.put("server.enabled", true);
    pt.put("server.host", "192.168.1.100");
    pt.put("server.port", 9100);
    pt.put("server.path", "/prometheus");
    pt.put("server.max_connections", 200);
    pt.put("system_metrics.enabled", false);
    pt.put("system_metrics.collection_interval", 10);
    pt.put("system_metrics.collect_cpu", false);
    pt.put("app_metrics.collect_http_requests", false);
    pt.put("export.format", "json");
    pt.put("export.namespace_prefix", "game");

    PrometheusConfig config;
    config.from_ptree(pt);

    BOOST_CHECK_EQUAL(config.server.host, "192.168.1.100");
    BOOST_CHECK_EQUAL(config.server.port, 9100);
    BOOST_CHECK_EQUAL(config.server.path, "/prometheus");
    BOOST_CHECK_EQUAL(config.server.max_connections, 200);
    BOOST_CHECK(!config.system_metrics.enabled);
    BOOST_CHECK_EQUAL(config.system_metrics.collection_interval, 10);
    BOOST_CHECK(!config.system_metrics.collect_cpu);
    BOOST_CHECK(!config.app_metrics.collect_http_requests);
    BOOST_CHECK_EQUAL(config.export_config.format, "json");
    BOOST_CHECK_EQUAL(config.export_config.namespace_prefix, "game");
}

BOOST_AUTO_TEST_SUITE_END()
