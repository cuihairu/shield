#define BOOST_TEST_MODULE CovHttpClient
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/net/http_client.hpp"
#include "shield/net/http_server.hpp"

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

using shield::net::HttpClient;
using shield::net::HttpClientOptions;
using shield::net::HttpClientResponse;
using shield::net::HttpFileField;
using shield::net::HttpRequest;
using shield::net::HttpResponse;
using shield::net::HttpServer;
using shield::net::HttpServerConfig;

namespace http = shield::net::http;

std::uint16_t reserve_ephemeral_port(boost::asio::io_context& io) {
    tcp::acceptor probe(io, tcp::endpoint(tcp::v4(), 0));
    return probe.local_endpoint().port();
}
// Local HTTP backend driven by the project's own server; every response is
// under full test control (status code, body).
struct LocalServer {
    boost::asio::io_context io;
    std::uint16_t port = 0;
    std::unique_ptr<HttpServer> server;

    HttpResponse ok_body(std::string tag) {
        HttpResponse r{http::status::ok, 11};
        r.body() = std::move(tag);
        return r;
    }

    void start() {
        port = reserve_ephemeral_port(io);
        HttpServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        server = std::make_unique<HttpServer>(cfg);

        server->get("/text", [this](const HttpRequest&) {
            return ok_body("plain-get");
        });
        server->get("/error500", [](const HttpRequest&) {
            HttpResponse r{http::status::internal_server_error, 11};
            r.body() = "boom";
            return r;
        });
        server->post("/echo", [](const HttpRequest& req) {
            HttpResponse r{http::status::ok, 11};
            r.set(http::field::content_type, "application/json");
            r.body() = req.body();
            return r;
        });
        server->route(shield::net::HttpMethod::PUT, "/echo",
                      [](const HttpRequest& req) {
                          HttpResponse r{http::status::ok, 11};
                          r.body() = req.body();
                          return r;
                      });
        server->route(shield::net::HttpMethod::PATCH, "/echo",
                      [](const HttpRequest& req) {
                          HttpResponse r{http::status::ok, 11};
                          r.body() = req.body();
                          return r;
                      });
        server->get("/file", [](const HttpRequest&) {
            HttpResponse r{http::status::ok, 11};
            r.body() = "DOWNLOADED-CONTENT";
            return r;
        });
        server->start();
    }
};

// A port that nothing listens on: connection refused error path.
std::uint16_t dead_port() {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);
    // Probe acceptor destroyed: the port is (almost certainly) free.
    return port;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(HttpClientCoverage)

BOOST_AUTO_TEST_CASE(RequestAgainstLocalServer) {
    shield::net::HttpClient::initialize();
    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    // Plain GET via the convenience wrapper.
    auto r1 = HttpClient::get(base + "/text", 5);
    BOOST_CHECK(r1.error.empty());
    BOOST_CHECK(r1.ok());
    BOOST_CHECK_EQUAL(r1.status_code, 200);
    BOOST_CHECK_EQUAL(r1.body, "plain-get");
    BOOST_CHECK(!r1.headers.empty());

    // Full options: custom headers, basic auth, redirects, relaxed SSL.
    HttpClientOptions opts;
    opts.method = "GET";
    opts.url = base + "/text";
    opts.headers["X-Test"] = "yes";
    opts.auth_basic_user = "user";
    opts.auth_basic_password = "pass";
    opts.follow_redirects = true;
    opts.max_redirects = 3;
    opts.verify_ssl = false;
    opts.ca_cert_path = "/etc/hostname";
    auto r2 = HttpClient::request(opts);
    BOOST_CHECK(r2.error.empty());
    BOOST_CHECK_EQUAL(r2.status_code, 200);
    BOOST_CHECK_EQUAL(r2.body, "plain-get");

    // POST with JSON body.
    auto r3 = HttpClient::post_json(base + "/echo", R"({"a":1})", 5);
    BOOST_CHECK(r3.error.empty());
    BOOST_CHECK_EQUAL(r3.status_code, 200);
    BOOST_CHECK_EQUAL(r3.body, R"({"a":1})");

    // PUT / PATCH with JSON bodies.
    auto r4 = HttpClient::put_json(base + "/echo", R"({"b":2})", 5);
    BOOST_CHECK(r4.error.empty());
    BOOST_CHECK_EQUAL(r4.body, R"({"b":2})");
    auto r5 = HttpClient::patch_json(base + "/echo", R"({"c":3})", 5);
    BOOST_CHECK(r5.error.empty());
    BOOST_CHECK_EQUAL(r5.body, R"({"c":3})");

    // DELETE.
    auto r6 = HttpClient::del(base + "/echo", 5);
    BOOST_CHECK(r6.error.empty());
    BOOST_CHECK_EQUAL(r6.status_code, 404);  // only /echo POST is routed

    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(EmptyBodyVariantForPost) {
    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    // POST without body exercises the empty-body branch.
    HttpClientOptions opts;
    opts.method = "POST";
    opts.url = base + "/echo";
    auto r = HttpClient::request(opts);
    BOOST_CHECK(r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 200);
    BOOST_CHECK(r.body.empty());

    // PUT without body.
    HttpClientOptions put_opts;
    put_opts.method = "PUT";
    put_opts.url = base + "/echo";
    auto r2 = HttpClient::request(put_opts);
    BOOST_CHECK_EQUAL(r2.status_code, 200);

    // PATCH without body.
    HttpClientOptions patch_opts;
    patch_opts.method = "PATCH";
    patch_opts.url = base + "/echo";
    auto r3 = HttpClient::request(patch_opts);
    BOOST_CHECK_EQUAL(r3.status_code, 200);

    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(ErrorPaths) {
    shield::net::HttpClient::initialize();

    // Malformed URL.
    auto r1 = HttpClient::get("not a url at all", 5);
    BOOST_CHECK(!r1.error.empty());
    BOOST_CHECK_EQUAL(r1.status_code, 0);

    // Connection refused: nothing listens on the port.
    auto r2 = HttpClient::get(
        "http://127.0.0.1:" + std::to_string(dead_port()) + "/", 2);
    BOOST_CHECK(!r2.error.empty());

    // Unreachable proxy.
    HttpClientOptions opts;
    opts.method = "GET";
    opts.url = "http://127.0.0.1:1/";
    opts.proxy = "http://127.0.0.1:1";
    opts.timeout_seconds = 2;
    auto r3 = HttpClient::request(opts);
    BOOST_CHECK(!r3.error.empty());
}

BOOST_AUTO_TEST_CASE(ServerErrorWithoutRetryKeepsStatus) {
    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    HttpClientOptions opts;
    opts.method = "GET";
    opts.url = base + "/error500";
    opts.retry_count = 0;
    opts.timeout_seconds = 5;
    auto r = HttpClient::request(opts);
    BOOST_CHECK(r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 500);
    BOOST_CHECK(!r.ok());

    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(RetryOnConnectionFailure) {
    // Retries against a dead port: every attempt fails fast, the retry delay
    // between attempts runs, and the final response carries the curl error.
    HttpClientOptions opts;
    opts.method = "GET";
    opts.url = "http://127.0.0.1:" + std::to_string(dead_port()) + "/";
    opts.retry_count = 2;
    opts.retry_delay_ms = 10;
    opts.timeout_seconds = 2;
    auto r = HttpClient::request(opts);
    BOOST_CHECK(!r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 0);
}

BOOST_AUTO_TEST_CASE(UploadMultipart) {
    shield::net::HttpClient::initialize();

    const std::string path =
        (std::filesystem::temp_directory_path() / "shield-cov-upload.txt")
            .string();
    {
        std::ofstream out(path);
        out << "upload-payload";
    }

    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    std::vector<HttpFileField> files;
    HttpFileField f;
    f.field_name = "file";
    f.file_path = path;
    f.content_type = "text/plain";
    files.push_back(f);

    std::unordered_map<std::string, std::string> fields;
    fields["name"] = "value";

    auto r = HttpClient::upload(base + "/echo", files, fields, 5);
    BOOST_CHECK(r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 200);
    BOOST_CHECK(r.body.find("upload-payload") != std::string::npos);

    // Upload to a dead endpoint surfaces the error.
    auto r2 = HttpClient::upload(
        "http://127.0.0.1:" + std::to_string(dead_port()) + "/up", files, {},
        2);
    BOOST_CHECK(!r2.error.empty());

    std::remove(path.c_str());
    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(DownloadToFile) {
    shield::net::HttpClient::initialize();

    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    const std::string out_path =
        (std::filesystem::temp_directory_path() / "shield-cov-download.txt")
            .string();
    std::remove(out_path.c_str());

    auto r = HttpClient::download(base + "/file", out_path, 5);
    BOOST_CHECK(r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 200);
    {
        std::ifstream in(out_path);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        BOOST_CHECK_EQUAL(content, "DOWNLOADED-CONTENT");
    }

    // Failed download removes the output file again.
    const std::string bad_path = "/tmp/shield-cov-download-bad.txt";
    auto r2 = HttpClient::download(
        "http://127.0.0.1:" + std::to_string(dead_port()) + "/file", bad_path,
        2);
    BOOST_CHECK(!r2.error.empty());
    BOOST_CHECK(std::remove(bad_path.c_str()) != 0);  // already removed

    // Unwritable target directory fails at fopen.
    auto r3 =
        HttpClient::download(base + "/file", "/nonexistent-dir/out.txt", 5);
    BOOST_CHECK(!r3.error.empty());
    BOOST_CHECK(r3.error.find("Failed to open output file") !=
                std::string::npos);

    std::remove(out_path.c_str());
    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(PostFormUrlEncoded) {
    shield::net::HttpClient::initialize();

    LocalServer srv;
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

    std::unordered_map<std::string, std::string> fields;
    fields["name"] = "value with spaces";
    fields["sym"] = "a&b=c";

    auto r = HttpClient::post_form(base + "/echo", fields, 5);
    BOOST_CHECK(r.error.empty());
    BOOST_CHECK_EQUAL(r.status_code, 200);
    BOOST_CHECK(r.body.find("name=value%20with%20spaces") != std::string::npos);
    BOOST_CHECK(r.body.find("sym=a%26b%3Dc") != std::string::npos);

    // Form post to a dead endpoint errors out.
    auto r2 = HttpClient::post_form(
        "http://127.0.0.1:" + std::to_string(dead_port()) + "/echo", fields, 2);
    BOOST_CHECK(!r2.error.empty());

    srv.server->stop();
}

BOOST_AUTO_TEST_CASE(GlobalCleanupRunsLast) {
    shield::net::HttpClient::cleanup();
    // Re-initialize so any framework teardown that logs still has a working
    // curl global state.
    shield::net::HttpClient::initialize();
}

BOOST_AUTO_TEST_SUITE_END()
