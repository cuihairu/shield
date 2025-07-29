#define BOOST_TEST_MODULE HttpHandlerTest
#include <boost/test/unit_test.hpp>
#include <boost/beast/http.hpp>
#include <string>

#include "shield/protocol/http_handler.hpp"

using namespace shield::protocol;
namespace http = boost::beast::http;

class MockHttpHandler : public HttpProtocolHandler {
public:
    MockHttpHandler() = default;
    
    // Expose protected methods for testing
    using HttpProtocolHandler::parse_request;
    using HttpProtocolHandler::build_response;
};

BOOST_AUTO_TEST_SUITE(HttpHandlerTests)

BOOST_AUTO_TEST_CASE(TestParseGetRequest) {
    MockHttpHandler handler;
    
    std::string raw_request = 
        "GET /api/player/info HTTP/1.1\r\n"
        "Host: localhost:8081\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "Content-Type: application/json\r\n"
        "\r\n";
    
    auto request = handler.parse_request(raw_request);
    
    BOOST_CHECK_EQUAL(request.method(), http::verb::get);
    BOOST_CHECK_EQUAL(request.target(), "/api/player/info");
    BOOST_CHECK_EQUAL(request[http::field::host], "localhost:8081");
    BOOST_CHECK_EQUAL(request[http::field::content_type], "application/json");
}

BOOST_AUTO_TEST_CASE(TestParsePostRequest) {
    MockHttpHandler handler;
    
    std::string json_body = "{\"action\":\"attack\",\"target\":\"enemy1\"}";
    std::string raw_request = 
        "POST /api/game/action HTTP/1.1\r\n"
        "Host: localhost:8081\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(json_body.size()) + "\r\n"
        "\r\n" + json_body;
    
    auto request = handler.parse_request(raw_request);
    
    BOOST_CHECK_EQUAL(request.method(), http::verb::post);
    BOOST_CHECK_EQUAL(request.target(), "/api/game/action");
    BOOST_CHECK_EQUAL(request.body(), json_body);
}

BOOST_AUTO_TEST_CASE(TestBuildSuccessResponse) {
    MockHttpHandler handler;
    
    std::string json_response = "{\"status\":\"success\",\"data\":{}}";
    auto response = handler.build_response(http::status::ok, json_response);
    
    BOOST_CHECK_EQUAL(response.result(), http::status::ok);
    BOOST_CHECK_EQUAL(response[http::field::content_type], "application/json");
    BOOST_CHECK_EQUAL(response.body(), json_response);
}

BOOST_AUTO_TEST_CASE(TestBuildErrorResponse) {
    MockHttpHandler handler;
    
    std::string error_message = "{\"error\":\"Invalid request\"}";
    auto response = handler.build_response(http::status::bad_request, error_message);
    
    BOOST_CHECK_EQUAL(response.result(), http::status::bad_request);
    BOOST_CHECK_EQUAL(response.body(), error_message);
}

BOOST_AUTO_TEST_SUITE_END()