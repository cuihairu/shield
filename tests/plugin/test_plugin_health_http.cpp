// health.http plugin tests: the /health and /ready endpoints must return
// well-formed JSON aggregating all registered checks. Regression anchor: the
// checks array once shipped with an unterminated key ("checks":[ instead of
// "checks":[) that every JSON parser rejects, so each endpoint test parses
// the body through nlohmann::json — that class of bug stays red.
#define BOOST_TEST_MODULE PluginHealthHttpTests
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/health.h"
#include "shield/plugin/host_api.h"

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Fixed probe port; the ctest registration holds RESOURCE_LOCK fixed_ports
// so this binary never races the other fixed-port suites under `ctest -j`.
constexpr unsigned short kPort = 18486;

// RAII wrapper: create the plugin instance from a config string, resolve the
// health interface, start the listener. shutdown() deletes the instance, so
// it must run exactly once per instance.
struct HealthInstance {
    shield_plugin_instance_v1* instance = nullptr;
    const shield_health_v1* health = nullptr;

    HealthInstance() {
        const std::string cfg =
            std::string("{\"bind_address\":\"127.0.0.1\",\"port\":") +
            std::to_string(kPort) + "}";
        shield_plugin_create_args_v1 args{};
        args.instance_id = "health.http.test";
        args.config_json = cfg.c_str();
        shield_error_v1 err{};
        BOOST_REQUIRE_EQUAL(
            shield_plugin_get_v1()->create(&args, &instance, &err), 0);
        BOOST_REQUIRE(instance != nullptr);
        BOOST_REQUIRE(instance->get_interface != nullptr);
        health = static_cast<const shield_health_v1*>(instance->get_interface(
            instance, SHIELD_HEALTH_INTERFACE, nullptr));
        BOOST_REQUIRE(health != nullptr);
        shield_error_v1 serr{};
        BOOST_REQUIRE_EQUAL(instance->start(instance, &serr), 0);
    }
    ~HealthInstance() {
        if (instance != nullptr) instance->shutdown(instance);
    }

    HealthInstance(const HealthInstance&) = delete;
    HealthInstance& operator=(const HealthInstance&) = delete;
};

// The plugin reinterprets the session back to its instance shell, so the
// session handle is the instance pointer (the plugin's documented contract;
// its connect() vtable entry is a no-op that returns null).
int register_probe(HealthInstance& inst, const char* name,
                   int (*check)(shield_health_check_result*, void*)) {
    return inst.health->register_check(
        reinterpret_cast<shield_health_session*>(inst.instance), name, check,
        nullptr);
}

int probe_ok(shield_health_check_result* r, void*) {
    r->status = SHIELD_HEALTH_OK;
    return 0;
}

int probe_degraded(shield_health_check_result* r, void*) {
    r->status = SHIELD_HEALTH_DEGRADED;
    return 0;
}

int probe_fail(shield_health_check_result* r, void*) {
    r->status = SHIELD_HEALTH_FAIL;
    r->message = strdup("db down");
    return 0;
}

http::response<http::string_body> http_get(const std::string& target) {
    net::io_context ioc;
    tcp::socket socket(ioc);
    socket.connect({net::ip::make_address("127.0.0.1"), kPort});
    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, "127.0.0.1");
    http::write(socket, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(socket, buf, res);
    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
    return res;
}

}  // namespace

// The motivating regression: liveness body must parse as JSON and carry the
// aggregated status plus one item per registered check. A degraded check
// degrades the aggregate but keeps 200.
BOOST_AUTO_TEST_CASE(HealthEndpointBodyIsWellFormedJsonWithAggregatedStatuses) {
    HealthInstance inst;
    BOOST_CHECK_EQUAL(register_probe(inst, "ok-probe", probe_ok), 0);
    BOOST_CHECK_EQUAL(register_probe(inst, "degraded-probe", probe_degraded),
                      0);

    auto res = http_get("/health");
    BOOST_CHECK_EQUAL(res.result_int(), 200u);
    nlohmann::json body = nlohmann::json::parse(res.body());
    BOOST_CHECK_EQUAL(body.at("status").get<std::string>(), "degraded");
    BOOST_REQUIRE(body.at("checks").is_array());
    BOOST_CHECK_EQUAL(body.at("checks").size(), 2u);
    BOOST_CHECK_EQUAL(body.at("checks")[0].at("name").get<std::string>(),
                      "ok-probe");
    BOOST_CHECK_EQUAL(body.at("checks")[0].at("status").get<std::string>(),
                      "ok");
    BOOST_CHECK_EQUAL(body.at("checks")[1].at("status").get<std::string>(),
                      "degraded");
    for (const auto& item : body.at("checks")) {
        BOOST_CHECK(item.at("latency_ms").is_number_integer());
    }
}

// A failing check maps the aggregate to fail and the endpoint to 503, with
// the check message escaped into the item.
BOOST_AUTO_TEST_CASE(FailingCheckMapsTo503AndFailAggregate) {
    HealthInstance inst;
    BOOST_CHECK_EQUAL(register_probe(inst, "fail-probe", probe_fail), 0);

    auto res = http_get("/ready");
    BOOST_CHECK_EQUAL(res.result_int(), 503u);
    nlohmann::json body = nlohmann::json::parse(res.body());
    BOOST_CHECK_EQUAL(body.at("status").get<std::string>(), "fail");
    BOOST_REQUIRE(body.at("checks").is_array());
    BOOST_CHECK_EQUAL(body.at("checks")[0].at("name").get<std::string>(),
                      "fail-probe");
    BOOST_CHECK_EQUAL(body.at("checks")[0].at("status").get<std::string>(),
                      "fail");
    BOOST_CHECK_EQUAL(body.at("checks")[0].at("message").get<std::string>(),
                      "db down");
}

// Unknown paths stay JSON ({"error":"not_found"}) with 404.
BOOST_AUTO_TEST_CASE(UnknownPathReturns404Json) {
    HealthInstance inst;
    auto res = http_get("/nope");
    BOOST_CHECK_EQUAL(res.result_int(), 404u);
    nlohmann::json body = nlohmann::json::parse(res.body());
    BOOST_CHECK_EQUAL(body.at("error").get<std::string>(), "not_found");
}

// check_all over the vtable returns one result per registered check with the
// plugin-allocated name/message strings handed back through free_result.
BOOST_AUTO_TEST_CASE(CheckAllVtableReturnsRegisteredResults) {
    HealthInstance inst;
    BOOST_CHECK_EQUAL(register_probe(inst, "ok-probe", probe_ok), 0);
    BOOST_CHECK_EQUAL(register_probe(inst, "fail-probe", probe_fail), 0);

    shield_health_check_result results[4] = {};
    int count = -1;
    BOOST_CHECK_EQUAL(
        inst.health->check_all(
            reinterpret_cast<shield_health_session*>(inst.instance), results, 4,
            &count),
        0);
    BOOST_CHECK_EQUAL(count, 2);
    BOOST_CHECK_EQUAL(results[0].status, SHIELD_HEALTH_OK);
    BOOST_CHECK_EQUAL(std::string(results[0].check_name), "ok-probe");
    BOOST_CHECK_EQUAL(results[1].status, SHIELD_HEALTH_FAIL);
    BOOST_CHECK_EQUAL(std::string(results[1].message), "db down");
    for (int i = 0; i < count; ++i) inst.health->free_result(&results[i]);
}
