// metrics.prometheus plugin tests: the /metrics endpoint must render the
// recorded counter/gauge/histogram series in Prometheus text format, and the
// embedded listener must shut down cleanly. Regression anchor: the listener
// once joined forever on shutdown (blocking accept not woken by close), so
// any hang here fails the whole suite by timeout.
#define BOOST_TEST_MODULE PluginMetricPrometheusTests
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/test/unit_test.hpp>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/metrics.h"

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Fixed probe port; the ctest registration holds RESOURCE_LOCK fixed_ports.
constexpr unsigned short kPort = 18487;

struct MetricsInstance {
    shield_plugin_instance_v1* instance = nullptr;
    const shield_metrics_v1* metrics = nullptr;

    MetricsInstance() {
        const std::string cfg =
            std::string("{\"bind_address\":\"127.0.0.1\",\"port\":") +
            std::to_string(kPort) + "}";
        shield_plugin_create_args_v1 args{};
        args.instance_id = "metrics.prometheus.test";
        args.config_json = cfg.c_str();
        shield_error_v1 err{};
        BOOST_REQUIRE_EQUAL(
            shield_plugin_get_v1()->create(&args, &instance, &err), 0);
        BOOST_REQUIRE(instance != nullptr);
        metrics = static_cast<const shield_metrics_v1*>(instance->get_interface(
            instance, SHIELD_METRICS_INTERFACE, nullptr));
        BOOST_REQUIRE(metrics != nullptr);
        shield_error_v1 serr{};
        BOOST_REQUIRE_EQUAL(instance->start(instance, &serr), 0);
    }
    ~MetricsInstance() {
        if (instance != nullptr) instance->shutdown(instance);
    }

    MetricsInstance(const MetricsInstance&) = delete;
    MetricsInstance& operator=(const MetricsInstance&) = delete;

    // Session = instance shell (the plugin's documented contract; connect()
    // is a no-op that returns null).
    shield_metrics_session* session() {
        return reinterpret_cast<shield_metrics_session*>(instance);
    }
};

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

bool contains_line(const std::string& text, const std::string& line) {
    return text.find(line + "\n") != std::string::npos;
}

}  // namespace

// Counters accumulate, gauges take the latest value, labels are normalized
// (sorted by key), and each family gets exactly one # TYPE header.
BOOST_AUTO_TEST_CASE(CounterGaugeHistogramRenderInPrometheusText) {
    MetricsInstance inst;
    const char* lk[] = {"route", "zone"};
    const char* lv[] = {"echo", "z1"};
    BOOST_CHECK_EQUAL(
        inst.metrics->counter_inc(inst.session(), "demo_requests_total", 1.0,
                                  lk, lv, 2),
        0);
    BOOST_CHECK_EQUAL(
        inst.metrics->counter_inc(inst.session(), "demo_requests_total", 1.0,
                                  lk, lv, 2),
        0);
    BOOST_CHECK_EQUAL(
        inst.metrics->gauge_set(inst.session(), "demo_queue_depth", 7.0,
                                nullptr, nullptr, 0),
        0);
    BOOST_CHECK_EQUAL(
        inst.metrics->gauge_set(inst.session(), "demo_queue_depth", 9.0,
                                nullptr, nullptr, 0),
        0);
    BOOST_CHECK_EQUAL(
        inst.metrics->histogram_observe(inst.session(), "demo_latency_ms", 3.5,
                                        nullptr, nullptr, 0),
        0);
    BOOST_CHECK_EQUAL(
        inst.metrics->histogram_observe(inst.session(), "demo_latency_ms", 1.5,
                                        nullptr, nullptr, 0),
        0);

    auto res = http_get("/metrics");
    BOOST_CHECK_EQUAL(res.result_int(), 200u);
    const std::string body = res.body();
    // Labels are rendered sorted by key ("route" before "zone") regardless
    // of insertion order.
    BOOST_CHECK(contains_line(body, "# TYPE demo_requests_total counter"));
    BOOST_CHECK(contains_line(
        body, "demo_requests_total{route=\"echo\",zone=\"z1\"} 2"));
    BOOST_CHECK(contains_line(body, "# TYPE demo_queue_depth gauge"));
    BOOST_CHECK(contains_line(body, "demo_queue_depth 9"));
    BOOST_CHECK(contains_line(body, "# TYPE demo_latency_ms histogram"));
    BOOST_CHECK(contains_line(body, "demo_latency_ms_sum 5"));
    BOOST_CHECK(contains_line(body, "demo_latency_ms_count 2"));
}

// record()/record_batch() cover the point forms, and SHIELD_METRIC_TIMER
// renders as a histogram family.
BOOST_AUTO_TEST_CASE(RecordVtablePointFormsAndBatch) {
    MetricsInstance inst;
    shield_metric_point counter{};
    counter.name = "batch_events_total";
    counter.type = SHIELD_METRIC_COUNTER;
    counter.value = 2.0;
    BOOST_CHECK_EQUAL(inst.metrics->record(inst.session(), &counter), 0);

    shield_metric_point timer{};
    timer.name = "batch_timer_ms";
    timer.type = SHIELD_METRIC_TIMER;
    timer.value = 4.0;
    shield_metric_point batch[2] = {counter, timer};
    BOOST_CHECK_EQUAL(inst.metrics->record_batch(inst.session(), batch, 2), 0);

    auto res = http_get("/metrics");
    BOOST_CHECK_EQUAL(res.result_int(), 200u);
    const std::string body = res.body();
    BOOST_CHECK(contains_line(body, "# TYPE batch_events_total counter"));
    BOOST_CHECK(contains_line(body, "batch_events_total 4"));
    BOOST_CHECK(contains_line(body, "# TYPE batch_timer_ms histogram"));
    BOOST_CHECK(contains_line(body, "batch_timer_ms_sum 4"));
    BOOST_CHECK(contains_line(body, "batch_timer_ms_count 1"));
}

// Unknown paths answer 404 with the plain-text body.
BOOST_AUTO_TEST_CASE(WrongPathReturns404Text) {
    MetricsInstance inst;
    auto res = http_get("/nope");
    BOOST_CHECK_EQUAL(res.result_int(), 404u);
    BOOST_CHECK_EQUAL(res.body(), "not found\n");
}

// A fresh registry renders 200 with an empty body (the documented kickstart
// behavior).
BOOST_AUTO_TEST_CASE(EmptyRegistryRendersEmptyBody) {
    MetricsInstance inst;
    auto res = http_get("/metrics");
    BOOST_CHECK_EQUAL(res.result_int(), 200u);
    BOOST_CHECK_EQUAL(res.body(), "");
}
