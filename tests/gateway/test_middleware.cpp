#define BOOST_TEST_MODULE MiddlewareTest
#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

#include "shield/gateway/middleware.hpp"

using namespace shield::gateway;

BOOST_AUTO_TEST_SUITE(MiddlewareChainTests)

BOOST_AUTO_TEST_CASE(TestEmptyChainCallsFinalHandler) {
    MiddlewareChain chain;
    GatewayRequest req;
    GatewayResponse resp;
    bool final_called = false;

    chain.execute(req, resp,
                  [&](GatewayRequest&, GatewayResponse&) { final_called = true; });

    BOOST_CHECK(final_called);
}

BOOST_AUTO_TEST_CASE(TestSingleMiddlewareCallsNext) {
    MiddlewareChain chain;
    std::vector<std::string> call_order;

    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("middleware");
        next();
    });

    GatewayRequest req;
    GatewayResponse resp;
    chain.execute(req, resp, [&](GatewayRequest&, GatewayResponse&) {
        call_order.push_back("final");
    });

    BOOST_REQUIRE_EQUAL(call_order.size(), 2);
    BOOST_CHECK_EQUAL(call_order[0], "middleware");
    BOOST_CHECK_EQUAL(call_order[1], "final");
}

BOOST_AUTO_TEST_CASE(TestMultipleMiddlewaresExecuteInOrder) {
    MiddlewareChain chain;
    std::vector<std::string> call_order;

    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("first");
        next();
    });
    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("second");
        next();
    });
    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("third");
        next();
    });

    GatewayRequest req;
    GatewayResponse resp;
    chain.execute(req, resp, [&](GatewayRequest&, GatewayResponse&) {
        call_order.push_back("final");
    });

    BOOST_REQUIRE_EQUAL(call_order.size(), 4);
    BOOST_CHECK_EQUAL(call_order[0], "first");
    BOOST_CHECK_EQUAL(call_order[1], "second");
    BOOST_CHECK_EQUAL(call_order[2], "third");
    BOOST_CHECK_EQUAL(call_order[3], "final");
}

BOOST_AUTO_TEST_CASE(TestMiddlewareShortCircuit) {
    MiddlewareChain chain;
    std::vector<std::string> call_order;

    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("first");
        next();
    });
    chain.use([&](GatewayRequest&, GatewayResponse& resp, std::function<void()>&) {
        call_order.push_back("short_circuit");
        resp.status_code = 403;
        resp.success = false;
        // Do NOT call next() — short-circuit
    });
    chain.use([&](GatewayRequest&, GatewayResponse&, std::function<void()>& next) {
        call_order.push_back("third");
        next();
    });

    GatewayRequest req;
    GatewayResponse resp;
    chain.execute(req, resp, [&](GatewayRequest&, GatewayResponse&) {
        call_order.push_back("final");
    });

    BOOST_REQUIRE_EQUAL(call_order.size(), 2);
    BOOST_CHECK_EQUAL(call_order[0], "first");
    BOOST_CHECK_EQUAL(call_order[1], "short_circuit");
    BOOST_CHECK_EQUAL(resp.status_code, 403);
    BOOST_CHECK(!resp.success);
}

BOOST_AUTO_TEST_CASE(TestMiddlewareModifiesRequest) {
    MiddlewareChain chain;

    chain.use([&](GatewayRequest& req, GatewayResponse&, std::function<void()>& next) {
        req.headers["X-Request-Id"] = "test-123";
        next();
    });

    GatewayRequest req;
    GatewayResponse resp;
    std::string captured_id;

    chain.execute(req, resp, [&](GatewayRequest& r, GatewayResponse&) {
        captured_id = r.headers["X-Request-Id"];
    });

    BOOST_CHECK_EQUAL(captured_id, "test-123");
}

BOOST_AUTO_TEST_CASE(TestMiddlewareModifiesResponse) {
    MiddlewareChain chain;

    chain.use([&](GatewayRequest&, GatewayResponse& resp, std::function<void()>& next) {
        next();
        resp.headers["X-Custom"] = "value";
    });

    GatewayRequest req;
    GatewayResponse resp;
    chain.execute(req, resp, [&](GatewayRequest&, GatewayResponse& r) {
        r.body = "response_body";
    });

    BOOST_CHECK_EQUAL(resp.body, "response_body");
    BOOST_CHECK_EQUAL(resp.headers["X-Custom"], "value");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(BuiltinMiddlewareTests)

BOOST_AUTO_TEST_CASE(TestCorsMiddlewareAddsHeaders) {
    auto mw = cors_middleware("https://example.com", "GET,POST", "Content-Type");

    GatewayRequest req;
    req.method = "GET";
    GatewayResponse resp;
    std::function<void()> next = []() {};

    mw(req, resp, next);

    BOOST_CHECK_EQUAL(resp.headers["Access-Control-Allow-Origin"],
                      "https://example.com");
    BOOST_CHECK_EQUAL(resp.headers["Access-Control-Allow-Methods"], "GET,POST");
    BOOST_CHECK_EQUAL(resp.headers["Access-Control-Allow-Headers"],
                      "Content-Type");
}

BOOST_AUTO_TEST_CASE(TestCorsMiddlewareShortCircuitsOptions) {
    auto mw = cors_middleware();

    GatewayRequest req;
    req.method = "OPTIONS";
    GatewayResponse resp;
    bool next_called = false;
    std::function<void()> next = [&]() { next_called = true; };

    mw(req, resp, next);

    BOOST_CHECK(!next_called);
    BOOST_CHECK_EQUAL(resp.status_code, 204);
    BOOST_CHECK(resp.success);
}

BOOST_AUTO_TEST_CASE(TestCorsMiddlewarePassesNonOptions) {
    auto mw = cors_middleware();

    GatewayRequest req;
    req.method = "GET";
    GatewayResponse resp;
    bool next_called = false;
    std::function<void()> next = [&]() { next_called = true; };

    mw(req, resp, next);

    BOOST_CHECK(next_called);
}

BOOST_AUTO_TEST_CASE(TestAuthMiddlewarePassesValidRequest) {
    auto mw = auth_middleware([](const GatewayRequest& req) {
        return req.headers.count("Authorization") > 0;
    });

    GatewayRequest req;
    req.headers["Authorization"] = "Bearer token";
    GatewayResponse resp;
    bool next_called = false;
    std::function<void()> next = [&]() { next_called = true; };

    mw(req, resp, next);

    BOOST_CHECK(next_called);
    BOOST_CHECK(resp.success);
}

BOOST_AUTO_TEST_CASE(TestAuthMiddlewareRejectsInvalidRequest) {
    auto mw = auth_middleware(
        [](const GatewayRequest&) { return false; });

    GatewayRequest req;
    GatewayResponse resp;
    bool next_called = false;
    std::function<void()> next = [&]() { next_called = true; };

    mw(req, resp, next);

    BOOST_CHECK(!next_called);
    BOOST_CHECK(!resp.success);
    BOOST_CHECK_EQUAL(resp.status_code, 401);
}

BOOST_AUTO_TEST_SUITE_END()
