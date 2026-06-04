#define BOOST_TEST_MODULE RuntimeDiagnosticsTest
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "shield/gateway/runtime_diagnostics.hpp"

using namespace shield::gateway;

BOOST_AUTO_TEST_SUITE(RuntimeDiagnosticsTests)

BOOST_AUTO_TEST_CASE(TestHealthJsonReturnsValidJson) {
    std::string result = RuntimeDiagnostics::health_json();
    BOOST_CHECK(!result.empty());

    auto j = nlohmann::json::parse(result);
    BOOST_CHECK_EQUAL(j["status"], "ok");
    BOOST_CHECK(j.contains("timestamp"));
    BOOST_CHECK(j["timestamp"].is_number());
}

BOOST_AUTO_TEST_CASE(TestConfigReloadInfoJsonReturnsValidJson) {
    std::string result = RuntimeDiagnostics::config_reload_info_json();
    BOOST_CHECK(!result.empty());

    auto j = nlohmann::json::parse(result);
    BOOST_CHECK(j.contains("reload"));
    BOOST_CHECK(j["reload"]["supported"].get<bool>());
    BOOST_CHECK(j["reload"].contains("trigger"));
    BOOST_CHECK(j["reload"].contains("scope"));
    BOOST_CHECK(j["reload"].contains("not_supported"));
    BOOST_CHECK(j["reload"]["scope"].is_array());
    BOOST_CHECK(j["reload"]["not_supported"].is_array());
}

BOOST_AUTO_TEST_CASE(TestHealthDetailedJsonReturnsValidJson) {
    std::string result = RuntimeDiagnostics::health_detailed_json();
    BOOST_CHECK(!result.empty());

    auto j = nlohmann::json::parse(result);
    BOOST_CHECK_EQUAL(j["status"], "ok");
    BOOST_CHECK(j.contains("timestamp"));
    BOOST_CHECK(j.contains("services"));
    BOOST_CHECK(j["services"].contains("count"));
    BOOST_CHECK(j["services"].contains("list"));
    BOOST_CHECK(j.contains("node"));
}

BOOST_AUTO_TEST_SUITE_END()
