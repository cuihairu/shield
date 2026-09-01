#define BOOST_TEST_MODULE CovOpsHttp
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/console/ops_http_handler.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_server.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace {

namespace fs = std::filesystem;

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

// Minimal HTTP/1.0 client: one fresh connection per request, read to EOF.
struct RawHttpClient {
    std::string host;
    uint16_t port = 0;

    void connect_target(const std::string& h, uint16_t p) {
        host = h;
        port = p;
    }

    std::string request(const std::string& raw,
                        std::chrono::milliseconds timeout) {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        try {
            socket.connect(boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address(host), port));
        } catch (...) {
            return {};
        }
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(raw), ec);
        if (ec) return {};

        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!socket.available()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::size_t n = socket.read_some(boost::asio::buffer(buf), ec);
            if (ec || n == 0) break;
            response.append(buf, n);
        }
        boost::system::error_code ignore;
        socket.close(ignore);
        return response;
    }

    std::string get(
        const std::string& path,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        return request("GET " + path + " HTTP/1.0\r\nHost: cov\r\n\r\n",
                       timeout);
    }

    std::string post(
        const std::string& path, const std::string& body,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        return request(
            "POST " + path +
                " HTTP/1.0\r\nHost: cov\r\n"
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body,
            timeout);
    }

    static int status_code(const std::string& response) {
        // "HTTP/1.0 200 OK"
        auto pos = response.find(' ');
        if (pos == std::string::npos) return -1;
        return std::atoi(response.c_str() + pos + 1);
    }

    static std::string body(const std::string& response) {
        auto pos = response.find("\r\n\r\n");
        if (pos == std::string::npos) return {};
        return response.substr(pos + 4);
    }
};

struct OpsFixture {
    caf::actor_system_config caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::unique_ptr<shield::lua::LuaRuntime> runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> manager;
    std::unique_ptr<shield::net::HttpServer> server;
    std::unique_ptr<shield::console::OpsHttpHandler> handler;
    uint16_t port = 0;

    OpsFixture() {
        system = std::make_unique<caf::actor_system>(caf_cfg);
        runtime = std::make_unique<shield::lua::LuaRuntime>();
        manager =
            std::make_unique<shield::lua::LuaServiceManager>(*runtime, *system);

        // Give the plugins endpoint at least one instance to list.
        prepare_one_instance();

        shield::config::global_config().set("cov.ops.str",
                                            std::string("value"));

        port = free_port();
        shield::net::HttpServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        server = std::make_unique<shield::net::HttpServer>(cfg);
        handler = std::make_unique<shield::console::OpsHttpHandler>(*manager,
                                                                    *runtime);
        handler->register_routes(*server);
        server->start();
        BOOST_REQUIRE(server->is_running());
    }

    ~OpsFixture() {
        if (server) server->stop();
    }

    void prepare_one_instance() {
        fs::path dir = fs::temp_directory_path() / "shield_cov_ops_plugins";
        fs::remove_all(dir);
        fs::create_directories(dir);
        auto& host = shield::plugin::global_host();
        std::string err;
        host.scan(dir.string());
        BOOST_REQUIRE_MESSAGE(host.catalog(err), err);
        shield::plugin::PluginConfig pc;
        pc.directory = dir.string();
        shield::plugin::InstanceDecl decl;
        decl.id = "cov_opt";
        decl.package = "absent.pkg";
        decl.required = false;
        pc.instances.push_back(decl);
        BOOST_REQUIRE_MESSAGE(host.plan_and_resolve(pc, err), err);
    }
};

}  // namespace

BOOST_FIXTURE_TEST_SUITE(OpsHttpTests, OpsFixture)

BOOST_AUTO_TEST_CASE(StatusEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/status", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    // No live service exists to host the ""-id task, so services times out.
    BOOST_CHECK(resp["data"]["services"] == "timeout");
    BOOST_CHECK(resp["data"]["plugins"].is_array());
    BOOST_CHECK(resp["data"]["plugins"].size() == 1);
}

BOOST_AUTO_TEST_CASE(ServicesListedWhenServiceIsAlive) {
    // With at least one live service, the ""-id ops task borrows its actor
    // and the services list resolves instead of timing out.
    const fs::path dir = fs::temp_directory_path() / "shield_cov_ops_svc";
    fs::create_directories(dir);
    std::ofstream(dir / "svc.lua") << "return { on_init = function() end }\n";
    auto spawned =
        manager->spawn((dir / "svc.lua").string(),
                       R"({"name":"cov_ops_svc","args":{},"config":{}})");
    BOOST_REQUIRE(spawned.success);

    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/services", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_array());
    BOOST_CHECK(resp["data"].size() >= 1);

    manager->shutdown_all("done");
}

BOOST_AUTO_TEST_CASE(ServicesEndpointTimesOut) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response =
        client.get("/ops/services", std::chrono::milliseconds(9000));
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 504);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_CASE(PluginsEndpoint) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);
    std::string response = client.get("/ops/plugins");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"].is_array());
    BOOST_CHECK(resp["data"].size() == 1);
    BOOST_CHECK(resp["data"][0]["id"] == "cov_opt");
}

BOOST_AUTO_TEST_CASE(ConfigEndpointVariants) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // No key -> hint text.
    std::string response = client.get("/ops/config");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"].get<std::string>().find("key") !=
                std::string::npos);

    // Unknown key -> null data.
    response = client.get("/ops/config?key=nope.key");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"].is_null());

    // Known key -> value, trailing parameters are stripped.
    response = client.get("/ops/config?key=cov.ops.str&other=1");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["data"] == "value");
}

BOOST_AUTO_TEST_CASE(EvalEndpointVariants) {
    RawHttpClient client;
    client.connect_target("127.0.0.1", port);

    // Invalid JSON body.
    std::string response = client.post("/ops/eval", "this is not json");
    BOOST_REQUIRE(!response.empty());
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    auto resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");

    // Missing 'code' field.
    response = client.post("/ops/eval", R"({"nope": 1})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);

    // Valid code -> 200 with data.
    response = client.post("/ops/eval", R"({"code": "return 6 * 7"})");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 200);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == nlohmann::json::array({42}));

    // Failing code -> 400 with error message.
    response =
        client.post("/ops/eval", R"json({"code": "error('bad code')"})json");
    BOOST_CHECK_EQUAL(RawHttpClient::status_code(response), 400);
    resp = nlohmann::json::parse(RawHttpClient::body(response));
    BOOST_CHECK(resp["type"] == "error");
}

BOOST_AUTO_TEST_SUITE_END()
