#define BOOST_TEST_MODULE CovConfig
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "shield/config/config.hpp"

namespace fs = std::filesystem;

using shield::config::Config;
using shield::config::ConfigValue;
using shield::config::RuntimeValidationOptions;

namespace {

const fs::path kTmpDir = "cov_cfg_tmp";
std::string g_script_abs;

void write_file(const fs::path& path, const std::string& content) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path);
    out << content;
}

struct GlobalFixture {
    GlobalFixture() {
        std::error_code ec;
        fs::remove_all(kTmpDir, ec);

        write_file(kTmpDir / "scripts" / "main.lua", "-- coverage script\n");
        g_script_abs = fs::absolute(kTmpDir / "scripts" / "main.lua").string();
        write_file(kTmpDir / "lua_scripts" / "lp.lua", "-- lua path script\n");
        write_file(kTmpDir / "srcdir" / "sd.lua", "-- source dir script\n");
        write_file(kTmpDir / "srcdir" / "cfg.yaml",
                   "app:\n  name: sourcedir\nactors:\n  - name: a\n    "
                   "script: sd.lua\n");
        write_file(kTmpDir / "bad.yaml", "a: [unclosed\n");
        write_file("cov_local.yaml", "app:\n  name: local\n");
        write_file("cov_cfg_cwd_s.lua", "-- cwd script\n");
    }

    ~GlobalFixture() {
        shield::config::reset_config();
        std::error_code ec;
        fs::remove_all(kTmpDir, ec);
        fs::remove("cov_local.yaml", ec);
        fs::remove("cov_cfg_cwd_s.lua", ec);
    }
};

RuntimeValidationOptions no_actors() {
    RuntimeValidationOptions opts;
    opts.require_actors = false;
    return opts;
}

// Loads yaml into a fresh global config and validates it.
struct ValidationRunner {
    RuntimeValidationOptions options;
    std::string error;

    bool run(const std::string& yaml) {
        shield::config::reset_config();
        shield::config::global_config().load_yaml_string(yaml);
        error.clear();
        return shield::config::validate_runtime_config(options, &error);
    }

    ~ValidationRunner() { shield::config::reset_config(); }
};

void expect_invalid(const std::string& yaml,
                    const RuntimeValidationOptions& opts,
                    const std::string& needle) {
    ValidationRunner runner;
    runner.options = opts;
    const bool ok = runner.run(yaml);
    BOOST_CHECK(!ok);
    BOOST_CHECK_NE(runner.error.find(needle), std::string::npos);
}

void expect_valid(const std::string& yaml,
                  const RuntimeValidationOptions& opts) {
    ValidationRunner runner;
    runner.options = opts;
    BOOST_CHECK(runner.run(yaml));
}

// Actor with a valid tcp listener; `block` is appended inside the actor
// mapping (4-space indent, e.g. "    network: 42\n").
std::string actor_cfg(const std::string& block) {
    return "app:\n  name: actorcase\nactors:\n  - name: a1\n    script: " +
           g_script_abs + "\n" + block;
}

// Actor whose network.protocol is described by `block` (8-space indent).
std::string proto_cfg(const std::string& block) {
    return actor_cfg(
        "    network:\n      tcp: \"127.0.0.1:18111\"\n      protocol:\n" +
        block);
}

}  // namespace

BOOST_GLOBAL_FIXTURE(GlobalFixture);

// ---------------------------------------------------------------- loading

BOOST_AUTO_TEST_CASE(LoadYamlStringBasics) {
    Config c;
    BOOST_CHECK(c.load_yaml_string(
        "i: 5\nf: 2.5\nb: true\ns: hello\nneg: -7\nnested:\n  child: deep\n  "
        "num: 9\n"));
    BOOST_CHECK_EQUAL(c.get_int("i"), 5);
    BOOST_CHECK_EQUAL(c.get_int("neg"), -7);
    BOOST_CHECK_EQUAL(c.get_double("f"), 2.5);
    BOOST_CHECK(c.get_bool("b"));
    BOOST_CHECK_EQUAL(c.get_string("s"), "hello");
    BOOST_CHECK_EQUAL(c.get_string("nested.child"), "deep");
    BOOST_CHECK_EQUAL(c.get_int("nested.num"), 9);
    BOOST_CHECK(c.has("nested.child"));
    BOOST_CHECK(!c.has("nested.missing"));

    // Non-map root: flatten walks nothing.
    BOOST_CHECK(c.load_yaml_string("42"));
    BOOST_CHECK(!c.has("i"));
    BOOST_CHECK(!c.has("anything"));
    BOOST_CHECK(c.load_yaml_string(""));

    Config inv;
    BOOST_CHECK(!inv.load_yaml_string("a: [unclosed\n"));
    BOOST_CHECK(!inv.load_yaml_string("{bad\n"));
}

BOOST_AUTO_TEST_CASE(SequenceHandling) {
    Config c;
    BOOST_CHECK(
        c.load_yaml_string("l: [a, b, c]\nm: [{x: 1}, {y: 2}]\nempty: []\n"));
    const auto l = c.get_string_array("l");
    BOOST_REQUIRE_EQUAL(l.size(), 3U);
    BOOST_CHECK_EQUAL(l[0], "a");
    BOOST_CHECK_EQUAL(l[2], "c");
    BOOST_CHECK(c.has("l"));
    BOOST_CHECK(c.has("empty"));
    BOOST_CHECK(c.get_string_array("empty").empty());
    // Sequences of non-scalars are not flattened into storage.
    BOOST_CHECK(!c.has("m"));
    BOOST_CHECK(c.get_string_array("m").empty());
    BOOST_CHECK(c.get_string_array("nope").empty());
    // Vector value falls through get_string to the default.
    BOOST_CHECK_EQUAL(c.get_string("l", "dft"), "dft");
}

BOOST_AUTO_TEST_CASE(MergeSemantics) {
    // Scalar overlay replaces a map at the same key.
    Config c;
    BOOST_CHECK(c.load_yaml_string("a:\n  b: 1\nkeep: 2\n"));
    BOOST_CHECK(c.load_yaml_string("a: 5\n"));
    BOOST_CHECK_EQUAL(c.get_int("a"), 5);
    BOOST_CHECK_EQUAL(c.get_int("keep"), 2);

    // Null overlay value keeps the base subtree.
    Config d;
    BOOST_CHECK(d.load_yaml_string("a:\n  x: 1\nb: 2\n"));
    BOOST_CHECK(d.load_yaml_string("a:\nc: 3\n"));
    BOOST_CHECK_EQUAL(d.get_int("a.x"), 1);
    BOOST_CHECK_EQUAL(d.get_int("b"), 2);
    BOOST_CHECK_EQUAL(d.get_int("c"), 3);

    // Deep merge overrides leaf keys only.
    Config e;
    BOOST_CHECK(e.load_yaml_string("n:\n  a: 1\n  keep: 2\n"));
    BOOST_CHECK(e.load_yaml_string("n:\n  a: 9\n"));
    BOOST_CHECK_EQUAL(e.get_int("n.a"), 9);
    BOOST_CHECK_EQUAL(e.get_int("n.keep"), 2);
}

BOOST_AUTO_TEST_CASE(LoadYamlFileVariants) {
    Config missing;
    BOOST_CHECK(!missing.load_yaml("/nonexistent/cov_xyz.yaml"));

    Config bad;
    BOOST_CHECK(!bad.load_yaml((kTmpDir / "bad.yaml").string()));

    Config ok;
    BOOST_CHECK(ok.load_yaml((kTmpDir / "srcdir" / "cfg.yaml").string()));
    BOOST_CHECK(ok.has("app.name"));
    BOOST_CHECK_EQUAL(ok.get_string("app.name"), "sourcedir");

    // Path without a directory component (parent is empty).
    Config local;
    BOOST_CHECK(local.load_yaml("cov_local.yaml"));
    BOOST_CHECK_EQUAL(local.get_string("app.name"), "local");
}

// ---------------------------------------------------------------- getters

BOOST_AUTO_TEST_CASE(TypedGetters) {
    Config c;
    BOOST_CHECK(c.load_yaml_string(
        "i: 5\nf: 2.5\nb: true\ns: hello\nsi: \"123\"\nsb: \"true\"\nsb1: "
        "\"1\"\nsby: \"yes\"\nsn: \"off\"\nsf: \"1.25\"\nsx: \"xyz\"\n"));

    // With yaml-cpp 0.9.0 plain scalars stay untagged, so YAML-loaded values
    // are stored as strings; conversions go through the string fallbacks.
    BOOST_CHECK_EQUAL(c.get_string("f"), "2.5");
    BOOST_CHECK_EQUAL(c.get_string("missing"), "");
    BOOST_CHECK_EQUAL(c.get_string("missing", "dft"), "dft");

    // Typed storage (and its conversions) is reachable via set().
    Config t;
    t.set("ti", ConfigValue(int64_t{5}));
    t.set("td", ConfigValue(1.25));
    t.set("tb", ConfigValue(true));
    BOOST_CHECK_EQUAL(t.get_string("ti"), "5");
    BOOST_CHECK_EQUAL(t.get_string("td"), "1.250000");
    BOOST_CHECK_EQUAL(t.get_string("tb"), "true");
    BOOST_CHECK_EQUAL(t.get_int("ti"), 5);
    BOOST_CHECK_EQUAL(t.get_int("td"), 1);
    BOOST_CHECK_EQUAL(t.get_double("ti"), 5.0);
    BOOST_CHECK_EQUAL(t.get_double("td"), 1.25);

    BOOST_CHECK_EQUAL(c.get_int("i"), 5);
    BOOST_CHECK_EQUAL(c.get_int("f"), 2);
    BOOST_CHECK_EQUAL(c.get_int("si"), 123);
    BOOST_CHECK_EQUAL(c.get_int("sx", 77), 77);
    BOOST_CHECK_EQUAL(c.get_int("missing", 9), 9);

    BOOST_CHECK_EQUAL(c.get_double("f"), 2.5);
    BOOST_CHECK_EQUAL(c.get_double("i"), 5.0);
    BOOST_CHECK_EQUAL(c.get_double("sf"), 1.25);
    BOOST_CHECK_EQUAL(c.get_double("sx", 9.5), 9.5);
    BOOST_CHECK_EQUAL(c.get_double("missing", 0.25), 0.25);

    BOOST_CHECK(c.get_bool("b"));
    BOOST_CHECK(c.get_bool("sb"));
    BOOST_CHECK(c.get_bool("sb1"));
    BOOST_CHECK(c.get_bool("sby"));
    BOOST_CHECK(!c.get_bool("sn", true));
    BOOST_CHECK(c.get_bool("missing", true));
    BOOST_CHECK(!c.get_bool("missing", false));
}

BOOST_AUTO_TEST_CASE(SetAndGet) {
    Config c;
    c.set("sv", ConfigValue(std::string("text")));
    c.set("iv", ConfigValue(int64_t{7}));
    c.set("dv", ConfigValue(1.25));
    c.set("bv", ConfigValue(true));
    c.set("lv", ConfigValue(std::vector<std::string>{"x", "y"}));

    const auto* sv = c.get_value("sv");
    BOOST_REQUIRE(sv != nullptr);
    BOOST_CHECK(std::holds_alternative<std::string>(*sv));
    const auto* iv = c.get_value("iv");
    BOOST_REQUIRE(iv != nullptr);
    BOOST_CHECK(std::holds_alternative<int64_t>(*iv));
    BOOST_CHECK_EQUAL(std::get<int64_t>(*iv), 7);
    const auto* dv = c.get_value("dv");
    BOOST_REQUIRE(dv != nullptr);
    BOOST_CHECK(std::holds_alternative<double>(*dv));
    BOOST_CHECK_EQUAL(std::get<double>(*dv), 1.25);
    const auto* bv = c.get_value("bv");
    BOOST_REQUIRE(bv != nullptr);
    BOOST_CHECK(std::holds_alternative<bool>(*bv));
    BOOST_CHECK(std::get<bool>(*bv));
    const auto* lv = c.get_value("lv");
    BOOST_REQUIRE(lv != nullptr);
    BOOST_CHECK(std::holds_alternative<std::vector<std::string>>(*lv));
    BOOST_CHECK(c.get_value("missing") == nullptr);

    BOOST_CHECK_EQUAL(c.get_string("iv"), "7");
    BOOST_CHECK(c.get_bool("bv"));
    BOOST_REQUIRE_EQUAL(c.get_string_array("lv").size(), 2U);
    BOOST_CHECK(c.has("sv"));
}

BOOST_AUTO_TEST_CASE(MergeConfigs) {
    Config a;
    BOOST_CHECK(a.load_yaml_string("x: 1\nkeep: base\n"));
    Config b;
    BOOST_CHECK(b.load_yaml_string("y: 2\nkeep: 2\n"));
    a.merge(b);
    BOOST_CHECK_EQUAL(a.get_int("x"), 1);
    BOOST_CHECK_EQUAL(a.get_int("keep"), 2);
    BOOST_CHECK(a.has("y"));
}

BOOST_AUTO_TEST_CASE(ToJson) {
    Config c;
    BOOST_CHECK(
        c.load_yaml_string("i: 5\nf: 2.5\nb: true\ns: hi\nl: [a, b]\n"));
    c.set("ti", ConfigValue(int64_t{7}));
    c.set("td", ConfigValue(1.25));
    c.set("tb", ConfigValue(true));
    const auto j = c.to_json();
    BOOST_CHECK_NE(j.find("\"ti\":7"), std::string::npos);
    BOOST_CHECK_NE(j.find("\"td\":1.25"), std::string::npos);
    BOOST_CHECK_NE(j.find("\"tb\":true"), std::string::npos);
    BOOST_CHECK_NE(j.find("\"s\":\"hi\""), std::string::npos);
    BOOST_CHECK_NE(j.find("\"i\":\"5\""), std::string::npos);
}

// ------------------------------------------------------- global functions

BOOST_AUTO_TEST_CASE(GlobalConvenienceFunctions) {
    shield::config::reset_config();
    auto& g = shield::config::global_config();
    BOOST_CHECK(g.load_yaml_string("i: 5\nf: 2.5\nb: true\ns: hi\n"));
    BOOST_CHECK_EQUAL(shield::config::get("s", "d"), "hi");
    BOOST_CHECK_EQUAL(shield::config::get("missing", "dflt"), "dflt");
    BOOST_CHECK_EQUAL(shield::config::get_int("i", 0), 5);
    BOOST_CHECK_EQUAL(shield::config::get_int("missing", 3), 3);
    BOOST_CHECK(shield::config::get_bool("b", false));
    BOOST_CHECK(shield::config::get_bool("missing", true));
    BOOST_CHECK_EQUAL(shield::config::get_double("f", 0.0), 2.5);
    BOOST_CHECK_EQUAL(shield::config::get_double("missing", 1.5), 1.5);
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(GlobalLifecycleAndReload) {
    shield::config::reset_config();
    auto* first = &shield::config::global_config();
    BOOST_CHECK(first != nullptr);
    first->load_yaml_string("k: v\n");
    shield::config::reset_config();
    BOOST_CHECK(!shield::config::global_config().has("k"));
    shield::config::reset_config();

    BOOST_CHECK(!shield::config::initialize_config("cov_missing_file_7.yaml"));
    BOOST_CHECK(shield::config::initialize_config("cov_local.yaml"));
    BOOST_CHECK(shield::config::global_config().has("app.name"));
    BOOST_CHECK(shield::config::reload_config());
    shield::config::reset_config();
}

// ---------------------------------------------------------- runtime_*

BOOST_AUTO_TEST_CASE(RuntimeActorsEmpty) {
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("app:\n  name: e\n");
    BOOST_CHECK(shield::config::runtime_actors().empty());
    shield::config::global_config().load_yaml_string("actors: 42\n");
    BOOST_CHECK(shield::config::runtime_actors().empty());
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(RuntimeActorsFull) {
    shield::config::reset_config();
    auto& g = shield::config::global_config();
    BOOST_CHECK(g.load_yaml_string(
        std::string("app:\n  name: full\nnet:\n  threads: 2\nactors:\n") +
        "  - name: a1\n    script: " + g_script_abs +
        "\n    instances: 3\n    required: 5\n"
        "    options:\n      map: true\n      num: 5\n      nul:\n"
        "    rpc:\n      routes:\n        - id: 1001\n          name: "
        "login\n          binding: do_login\n          direction: c2s\n"
        "        - id: 2001\n          name: login_result\n          "
        "binding: push_result\n          direction: s2c\n"
        "  - name: a2\n    script: s2.lua\n    required: true\n"
        "    options: [1, 2.5, true, text]\n"
        "    network:\n      tcp: \"127.0.0.1:19001\"\n      "
        "max_connections: 100\n      max_connections_per_ip: 10\n      "
        "max_frame_size: 4096\n      max_session_send_queue: 50\n      "
        "read_idle_timeout: 30000\n      protocol:\n        envelope:\n        "
        "  "
        "type: lenprefix\n"
        "  - name: a3\n    script: s3.lua\n    options: true\n"
        "  - name: a4\n    script: s4.lua\n    options: 5\n"
        "  - name: a5\n    script: s5.lua\n    options: 2.5\n"
        "  - name: a6\n    script: s6.lua\n    options: hello\n"
        "  - name: a7\n    script: s7.lua\n    network: {}\n"));

    const auto actors = shield::config::runtime_actors();
    BOOST_REQUIRE_EQUAL(actors.size(), 7U);

    BOOST_CHECK_EQUAL(actors[0].name, "a1");
    BOOST_CHECK_EQUAL(actors[0].script, g_script_abs);
    BOOST_CHECK_EQUAL(actors[0].instances, 3);
    BOOST_CHECK(actors[0].required);  // `required: 5` falls back to true
    BOOST_CHECK_NE(actors[0].options_json.find("\"num\":5"), std::string::npos);
    BOOST_CHECK_NE(actors[0].options_json.find("\"nul\":null"),
                   std::string::npos);
    // rpc.routes flows through to the spawn-time descriptor source.
    BOOST_CHECK_NE(actors[0].rpc_routes_json.find("\"binding\":\"do_login\""),
                   std::string::npos);
    BOOST_CHECK_NE(actors[0].rpc_routes_json.find("\"direction\":\"s2c\""),
                   std::string::npos);
    BOOST_CHECK_EQUAL(actors[1].rpc_routes_json, "[]");

    BOOST_CHECK_EQUAL(actors[1].network_tcp, "127.0.0.1:19001");
    BOOST_CHECK_EQUAL(actors[1].max_connections, 100U);
    BOOST_CHECK_EQUAL(actors[1].max_connections_per_ip, 10U);
    BOOST_CHECK_EQUAL(actors[1].max_frame_size, 4096U);
    BOOST_CHECK_EQUAL(actors[1].max_session_send_queue, 50U);
    BOOST_CHECK_EQUAL(actors[1].read_idle_timeout_ms, 30000U);
    BOOST_CHECK(actors[1].network_protocol_enabled);
    BOOST_CHECK_NE(actors[1].network_protocol_json.find("lenprefix"),
                   std::string::npos);
    BOOST_CHECK_NE(actors[1].options_json.find("\"text\""), std::string::npos);
    BOOST_CHECK_EQUAL(actors[1].instances, 1);
    BOOST_CHECK(actors[1].required);

    BOOST_CHECK_EQUAL(actors[2].options_json, "true");
    BOOST_CHECK_NE(actors[3].options_json.find("5"), std::string::npos);
    BOOST_CHECK_NE(actors[4].options_json.find("2.5"), std::string::npos);
    BOOST_CHECK_EQUAL(actors[5].options_json, "\"hello\"");

    BOOST_CHECK(actors[6].network_tcp.empty());
    BOOST_CHECK_EQUAL(actors[6].max_connections, 0U);
    BOOST_CHECK_EQUAL(actors[6].read_idle_timeout_ms, 0U);
    BOOST_CHECK(!actors[6].network_protocol_enabled);
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(RuntimeNetThreads) {
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("net:\n  threads: 4\n");
    BOOST_CHECK_EQUAL(shield::config::runtime_net_threads(), 4U);
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("net:\n  threads: -1\n");
    BOOST_CHECK_EQUAL(shield::config::runtime_net_threads(), 0U);
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("net:\n  threads: abc\n");
    BOOST_CHECK_EQUAL(shield::config::runtime_net_threads(), 0U);
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("net: {}\n");
    BOOST_CHECK_EQUAL(shield::config::runtime_net_threads(), 0U);
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string("app:\n  name: n\n");
    BOOST_CHECK_EQUAL(shield::config::runtime_net_threads(), 0U);
    shield::config::reset_config();
}

// ------------------------------------------------ validate_runtime_config

BOOST_AUTO_TEST_CASE(ValidateOptionalModules) {
    expect_invalid("app:\n  name: o\ncluster:\n  enabled: true\n", no_actors(),
                   "shield_cluster");
    expect_invalid("app:\n  name: o\nglobal:\n  enabled: true\n", no_actors(),
                   "shield_global");
    expect_invalid("app:\n  name: o\nplayer:\n  enabled: true\n", no_actors(),
                   "shield_player");
    expect_invalid("app:\n  name: o\nserver_manager:\n  enabled: true\n",
                   no_actors(), "shield_server");
    expect_invalid("app:\n  name: o\nops:\n  enabled: true\n", no_actors(),
                   "shield_ops");

    RuntimeValidationOptions cluster_on = no_actors();
    cluster_on.cluster_enabled = true;
    expect_valid("app:\n  name: o\ncluster:\n  enabled: true\n", cluster_on);
}

BOOST_AUTO_TEST_CASE(ValidateApp) {
    expect_invalid("", no_actors(), "app.name is required");
    expect_invalid("app: {}", no_actors(), "app.name is required");
    expect_invalid("app: 42", no_actors(), "app.name is required");
    expect_invalid("app:\n  name: \"\"", no_actors(), "app.name is required");
    expect_invalid("app:\n  name: " + std::string(65, 'a'), no_actors(),
                   "app.name must be 1-64 characters");
    expect_valid("app:\n  name: ok", no_actors());
}

BOOST_AUTO_TEST_CASE(ValidateLogLevel) {
    expect_invalid("app:\n  name: l\nlog:\n  level: verbose\n", no_actors(),
                   "log.level must be");
    expect_valid("app:\n  name: l\nlog:\n  level: warn\n", no_actors());
    expect_valid("app:\n  name: l\nlog:\n  level: debug\n", no_actors());
    expect_valid("app:\n  name: l\n", no_actors());
}

BOOST_AUTO_TEST_CASE(ValidateLua) {
    expect_invalid("app:\n  name: m\nlua:\n  vm:\n    mode: shared\n",
                   no_actors(), "per_service");
    expect_valid("app:\n  name: m\nlua:\n  vm:\n    mode: per_service\n",
                 no_actors());
    expect_invalid("app:\n  name: m\nlua:\n  cache:\n    max_size: 0\n",
                   no_actors(), "lua.cache.max_size must be between");
    expect_invalid("app:\n  name: m\nlua:\n  cache:\n    max_size: abc\n",
                   no_actors(), "lua.cache.max_size must be an integer");
    expect_invalid("app:\n  name: m\nlua:\n  cache:\n    ttl_seconds: 99999\n",
                   no_actors(), "lua.cache.ttl_seconds must be between");
    expect_valid(
        "app:\n  name: m\nlua:\n  cache:\n    max_size: 100\n    ttl_seconds: "
        "60\n",
        no_actors());
}

BOOST_AUTO_TEST_CASE(ValidateActorsShape) {
    const auto opts = RuntimeValidationOptions{};
    expect_invalid("app:\n  name: a\n", opts, "actors must contain at least");
    expect_invalid("app:\n  name: a\nactors: []\n", opts,
                   "actors must contain at least");
    expect_invalid("app:\n  name: a\nactors: 42\n", opts,
                   "actors must contain at least");
    expect_invalid("app:\n  name: a\nactors:\n  - 42\n", opts,
                   "actors[0] must be a map");
    expect_invalid(
        "app:\n  name: a\nactors:\n  - script: " + g_script_abs + "\n", opts,
        "actors[0].name is required");
    expect_invalid("app:\n  name: a\nactors:\n  - name: \"\"\n    script: " +
                       g_script_abs + "\n",
                   opts, "actors[0].name is required");
    expect_invalid(
        "app:\n  name: a\nactors:\n  - name: w\n    script: " + g_script_abs +
            "\n  - name: w\n    script: " + g_script_abs + "\n",
        opts, "unique");
    expect_invalid("app:\n  name: a\nactors:\n  - name: a1\n", opts,
                   "script is required");
    expect_invalid("app:\n  name: a\nactors:\n  - name: a1\n    script: \"\"\n",
                   opts, "script is required");
    expect_invalid("app:\n  name: a\nactors:\n  - name: a1\n    script: " +
                       g_script_abs + "\n    instances: -1\n",
                   opts, "instances must be >= 0");
    expect_invalid(
        "app:\n  name: a\nactors:\n  - name: a1\n    script: " + g_script_abs +
            "\n    restart:\n      policy: sometimes\n",
        opts, "restart.policy must be");
    // Non-integer instances is tolerated (falls back to 1).
    expect_valid("app:\n  name: a\nactors:\n  - name: a1\n    script: " +
                     g_script_abs + "\n    instances: abc\n",
                 opts);
    expect_valid(
        "app:\n  name: a\nactors:\n  - name: a1\n    script: " + g_script_abs +
            "\n    restart:\n      policy: "
            "on-failure\n",
        opts);
}

BOOST_AUTO_TEST_CASE(ValidateScriptResolution) {
    const auto opts = RuntimeValidationOptions{};
    expect_valid("app:\n  name: r\nactors:\n  - name: a\n    script: " +
                     g_script_abs + "\n",
                 opts);
    expect_valid(
        "app:\n  name: r\nactors:\n  - name: a\n    script: "
        "cov_cfg_cwd_s.lua\n",
        opts);
    expect_invalid(
        "app:\n  name: r\nactors:\n  - name: a\n    script: "
        "cov_missing_9.lua\n",
        opts, "does not exist");
    expect_valid(
        "app:\n  name: r\nlua:\n  script_path: cov_cfg_tmp/lua_scripts\n"
        "actors:\n  - name: a\n    script: lp.lua\n",
        opts);

    // Resolution relative to the config file's directory (source_dir).
    shield::config::reset_config();
    BOOST_CHECK(shield::config::initialize_config(
        (kTmpDir / "srcdir" / "cfg.yaml").string()));
    std::string err;
    BOOST_CHECK(shield::config::validate_runtime_config(opts, &err));
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(ValidateNetworkBasics) {
    const auto opts = RuntimeValidationOptions{};
    expect_invalid(actor_cfg("    network: 42\n"), opts,
                   "network must be a map");
    expect_invalid(actor_cfg("    network:\n      udp: \"0.0.0.0:9000\"\n"),
                   opts, "only supports tcp");
    expect_invalid(actor_cfg("    network:\n      kcp: \"0.0.0.0:9000\"\n"),
                   opts, "only supports tcp");
    expect_invalid(
        actor_cfg("    network:\n      websocket: \"0.0.0.0:9000\"\n"), opts,
        "only supports tcp");
    expect_invalid(actor_cfg("    network:\n      tcp:\n        a: 1\n"), opts,
                   "must be a string host:port");
    expect_invalid(actor_cfg("    network:\n      tcp: nohostport\n"), opts,
                   "must be host:port");
    expect_invalid(actor_cfg("    network:\n      tcp: \":9000\"\n"), opts,
                   "must be host:port");
    expect_invalid(actor_cfg("    network:\n      tcp: \"127.0.0.1:\"\n"), opts,
                   "must be host:port");
    expect_invalid(actor_cfg("    network:\n      tcp: \"127.0.0.1:abc\"\n"),
                   opts, "port must be between");
    expect_invalid(actor_cfg("    network:\n      tcp: \"127.0.0.1:99999\"\n"),
                   opts, "port must be between");
    expect_invalid(actor_cfg("    instances: 2\n    network:\n      tcp: "
                             "\"127.0.0.1:18001\"\n      protocol:\n"
                             "        envelope:\n          type: lenprefix\n"
                             "        body:\n          codec: json\n"),
                   opts, "requires instances to be 1");
    expect_invalid(
        actor_cfg("    network:\n      tcp: \"127.0.0.1:18001\"\n      "
                  "protocol:\n        envelope:\n          type: lenprefix\n"
                  "        body:\n          codec: json\n"
                  "      max_connections: 0\n"),
        opts, "max_connections must be between");
    expect_invalid(
        actor_cfg("    network:\n      tcp: \"127.0.0.1:18001\"\n      "
                  "protocol:\n        envelope:\n          type: lenprefix\n"
                  "        body:\n          codec: json\n"
                  "      max_connections: abc\n"),
        opts, "max_connections must be an integer");
    expect_invalid(actor_cfg("    network:\n      protocol: 42\n"), opts,
                   "network.protocol must be a map");
    expect_invalid(
        actor_cfg("    network:\n      tcp: \"127.0.0.1:18001\"\n      "
                  "protocol: {}\n"),
        opts, "must not be empty");
    expect_invalid(actor_cfg("    network:\n      tcp: \"127.0.0.1:18001\"\n"),
                   opts, "requires network.protocol");
    expect_valid(
        actor_cfg(
            "    network:\n      tcp: \"127.0.0.1:18001\"\n      "
            "protocol:\n        envelope:\n          type: lenprefix\n"
            "        body:\n          codec: json\n      "
            "max_connections: 10\n      max_connections_per_ip: 2\n      "
            "max_frame_size: 1024\n      max_session_send_queue: 8\n      "
            "read_idle_timeout: 100\n"),
        opts);
    expect_valid(actor_cfg("    network: {}\n"), opts);
}

BOOST_AUTO_TEST_CASE(ValidateProtocolBody) {
    const auto opts = RuntimeValidationOptions{};
    expect_valid(proto_cfg("        body:\n          codec: raw\n"), opts);

    expect_invalid(proto_cfg("        name:\n          k: v\n"), opts,
                   "protocol.name must be a string");
    expect_invalid(proto_cfg("        body: 42\n"), opts, "body must be a map");
    expect_invalid(proto_cfg("        body:\n          codec: bogus\n"), opts,
                   "body.codec has an unsupported value");
    expect_invalid(proto_cfg("        body:\n          codec: raw\n          "
                             "catalog:\n            k: v\n"),
                   opts, "body.catalog must be a string");
    expect_invalid(proto_cfg("        body:\n          codec: raw\n          "
                             "provider: \"\"\n"),
                   opts, "body.provider must not be empty");
    expect_invalid(proto_cfg("        body:\n          codec: raw\n          "
                             "provider:\n            k: v\n"),
                   opts, "body.provider must be a string");
    expect_valid(proto_cfg("        body:\n          codec: raw\n          "
                           "catalog: cat.json\n          provider: p1\n"),
                 opts);
}

BOOST_AUTO_TEST_CASE(ValidateProtocolEnvelope) {
    const auto opts = RuntimeValidationOptions{};
    const std::string prefix = "        envelope:\n";
    expect_invalid(proto_cfg("        envelope: 42\n"), opts,
                   "envelope must be a map");
    expect_invalid(proto_cfg(prefix + "          type:\n            k: v\n"),
                   opts, "envelope.type must be a string");
    expect_invalid(proto_cfg(prefix + "          type: bogus\n"), opts,
                   "envelope.type has an unsupported value");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "endian: bogus\n"),
                   opts, "envelope.endian has an unsupported value");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "length_bytes: 999\n"),
                   opts, "envelope.length_bytes must be between");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "length_bytes: abc\n"),
                   opts, "envelope.length_bytes must be an integer");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "route_id_bytes: -1\n"),
                   opts, "envelope.route_id_bytes must be between");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "max_frame_size: 20000000\n"),
                   opts, "envelope.max_frame_size must be between");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "length_includes_header: 123\n"),
                   opts, "length_includes_header must be a bool");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "delimiter: \"\"\n"),
                   opts, "envelope.delimiter must not be empty");
    expect_invalid(proto_cfg(prefix + "          type: lenprefix\n          "
                                      "delimiter:\n            k: v\n"),
                   opts, "envelope.delimiter must be a string");
    expect_valid(
        proto_cfg(prefix + "          type: lenprefix\n          endian: "
                           "little\n          length_bytes: 4\n          "
                           "route_id_bytes: 2\n          max_frame_size: "
                           "65536\n          length_includes_header: true\n"
                           "          delimiter: \"\\n\"\n"),
        opts);
}

BOOST_AUTO_TEST_CASE(ValidateProtocolRouting) {
    const auto opts = RuntimeValidationOptions{};
    const std::string prefix = "        routing:\n";
    expect_invalid(proto_cfg("        routing: 42\n"), opts,
                   "routing must be a map");
    expect_invalid(proto_cfg(prefix + "          source: bogus\n"), opts,
                   "routing.source has an unsupported value");
    expect_invalid(proto_cfg(prefix + "          source: none\n          "
                                      "unknown_route_action: bogus\n"),
                   opts, "unknown_route_action has an unsupported value");
    expect_invalid(proto_cfg(prefix + "          source: none\n          "
                                      "default_action: bogus\n"),
                   opts, "default_action has an unsupported value");
    expect_invalid(proto_cfg(prefix + "          source: none\n          "
                                      "lazy_decode: 5\n"),
                   opts, "routing.lazy_decode must be a bool");
    expect_valid(
        proto_cfg(prefix + "          source: header.route_id\n          "
                           "unknown_route_action: decode\n          "
                           "default_action: drop\n          "
                           "decode_body_route: true\n          "
                           "decode_before_dispatch: false\n"),
        opts);
}

BOOST_AUTO_TEST_CASE(ProtocolRoutesKeyIsRejected) {
    // Inline network.protocol.routes was folded into the RPC descriptor set:
    // the key is rejected outright (pre-1.0, no compatibility shim).
    const auto opts = RuntimeValidationOptions{};
    expect_invalid(proto_cfg("        routes: []\n"), opts, "was removed");
    expect_invalid(
        proto_cfg("        routes:\n          - id: 1\n            name: "
                  "login\n"),
        opts, "actors[].rpc.routes");
}

BOOST_AUTO_TEST_CASE(ValidateActorRpcRoutes) {
    const auto opts = RuntimeValidationOptions{};
    expect_invalid(actor_cfg("    rpc: 42\n"), opts, "rpc must be a map");

    const std::string prefix = "    rpc:\n      routes:\n";
    // Sequence items sit at 8 spaces; their map keys at 10.
    const std::string r1 = prefix + "        - id: 1\n          binding: b\n";
    expect_invalid(actor_cfg("    rpc:\n      routes: 42\n"), opts,
                   "routes must be an array");
    expect_invalid(actor_cfg(prefix + "        - 42\n"), opts,
                   "routes[0] must be a map");
    expect_invalid(actor_cfg(prefix + "        - name: noid\n" + r1), opts,
                   "routes[0].id is required and must be an integer");
    expect_invalid(actor_cfg(prefix + "        - id: abc\n" + r1), opts,
                   "routes[0].id is required and must be an integer");
    expect_invalid(actor_cfg(prefix + "        - id: 0\n" + r1), opts,
                   "routes[0].id must be >= 1");
    expect_invalid(actor_cfg(r1 + "        - id: 1\n          binding: c\n"),
                   opts, "duplicate id");
    expect_invalid(actor_cfg(r1 + "          direction: sideways\n"), opts,
                   "direction has an unsupported value");
    expect_invalid(actor_cfg(r1 + "          action: bogus\n"), opts,
                   "action has an unsupported value");
    expect_invalid(actor_cfg(r1 + "          requires_auth: 7\n"), opts,
                   "requires_auth must be a bool");
    expect_invalid(actor_cfg(r1 + "          lazy_decode: 7\n"), opts,
                   "lazy_decode must be a bool");
    expect_invalid(
        actor_cfg(prefix + "        - id: 1\n          binding: \"\"\n"), opts,
        "binding must not be empty");
    expect_invalid(
        actor_cfg(prefix + "        - id: 1\n          name: n1\n          "
                           "owner_service:\n            k: v\n          "
                           "binding: b\n"),
        opts, "owner_service must be a string");
    expect_invalid(actor_cfg(prefix + "        - id: 1\n"
                                      "          name: dup\n"
                                      "          binding: b\n"
                                      "        - id: 2\n"
                                      "          name: dup\n"
                                      "          binding: c\n"),
                   opts, "duplicate name");
    expect_valid(actor_cfg(prefix + "        - id: 1\n"
                                    "          name: login\n"
                                    "          binding: do_login\n"
                                    "          direction: c2s\n"
                                    "          requires_auth: false\n"
                                    "          action: decode_local\n"
                                    "          lazy_decode: true\n"
                                    "          request_codec: json\n"
                                    "          request_schema: login.req\n"
                                    "          response_schema: login.resp\n"
                                    "        - id: 2\n"
                                    "          name: push\n"
                                    "          binding: push_helper\n"
                                    "          direction: s2c\n"),
                 opts);
    expect_valid(actor_cfg("    rpc: {}\n"), opts);
}

BOOST_AUTO_TEST_CASE(ValidateShutdown) {
    expect_invalid(
        "app:\n  name: s\nshutdown:\n  timeout:\n    total: 100\n  "
        "  service_drain: 200\n",
        no_actors(), "greater than service_drain");
    expect_invalid(
        "app:\n  name: s\nshutdown:\n  timeout:\n    total: 100\n  "
        "  service_stop: 500\n",
        no_actors(), "greater than service_stop");
    expect_invalid(
        "app:\n  name: s\nshutdown:\n  timeout:\n    total: 100\n  "
        "  plugin_shutdown: 500\n",
        no_actors(), "greater than plugin_shutdown");
    expect_valid(
        "app:\n  name: s\nshutdown:\n  timeout:\n    total: 1000\n   "
        " service_drain: 100\n    service_stop: 100\n    "
        "plugin_shutdown: 50\n",
        no_actors());
    expect_valid("app:\n  name: s\nshutdown:\n  timeout: 42\n", no_actors());
    expect_valid(
        "app:\n  name: s\nshutdown:\n  timeout:\n    service_drain: "
        "10\n",
        no_actors());
}

BOOST_AUTO_TEST_CASE(ValidateNet) {
    expect_invalid("app:\n  name: n\nnet:\n  threads: 999\n", no_actors(),
                   "net.threads must be between");
    expect_invalid("app:\n  name: n\nnet:\n  threads: abc\n", no_actors(),
                   "net.threads must be an integer");
    expect_valid("app:\n  name: n\nnet:\n  threads: 4\n", no_actors());
    expect_valid("app:\n  name: n\nnet: {}\n", no_actors());
}

BOOST_AUTO_TEST_CASE(ValidateFullValidConfig) {
    const std::string yaml =
        "app:\n"
        "  name: full_demo\n"
        "log:\n"
        "  level: info\n"
        "lua:\n"
        "  vm:\n"
        "    mode: per_service\n"
        "  cache:\n"
        "    max_size: 128\n"
        "    ttl_seconds: 60\n"
        "net:\n"
        "  threads: 2\n"
        "shutdown:\n"
        "  timeout:\n"
        "    total: 5000\n"
        "    service_drain: 1000\n"
        "    service_stop: 1000\n"
        "    plugin_shutdown: 500\n"
        "actors:\n"
        "  - name: gateway\n"
        "    script: " +
        g_script_abs +
        "\n"
        "    instances: 1\n"
        "    required: true\n"
        "    restart:\n"
        "      policy: always\n"
        "    options:\n"
        "      verbose: true\n"
        "    network:\n"
        "      tcp: \"127.0.0.1:18222\"\n"
        "      max_connections: 100\n"
        "      max_connections_per_ip: 5\n"
        "      max_frame_size: 65536\n"
        "      max_session_send_queue: 128\n"
        "      read_idle_timeout: 30000\n"
        "      protocol:\n"
        "        name: game1\n"
        "        body:\n"
        "          codec: json\n"
        "          catalog: catalog.json\n"
        "          provider: provider1\n"
        "        envelope:\n"
        "          type: lenprefix\n"
        "          endian: little\n"
        "          length_bytes: 4\n"
        "          route_id_bytes: 2\n"
        "          max_frame_size: 65536\n"
        "          length_includes_header: true\n"
        "          delimiter: \"\\n\"\n"
        "        routing:\n"
        "          source: header.route_id\n"
        "          unknown_route_action: decode\n"
        "          default_action: drop\n"
        "          decode_body_route: true\n"
        "          decode_before_dispatch: false\n"
        "          lazy_decode: false\n"
        "    rpc:\n"
        "      routes:\n"
        "        - id: 1\n"
        "          name: login\n"
        "          binding: do_login\n"
        "          lazy_decode: false\n"
        "        - id: 2\n"
        "          name: logout\n"
        "          binding: do_logout\n"
        "          action: drop\n";
    expect_valid(yaml, RuntimeValidationOptions{});

    // require_actors = false skips the actor section entirely.
    expect_valid("app:\n  name: solo\n", no_actors());
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (purely additive).
// ---------------------------------------------------------------------------

// Typed getters hitting the variant alternatives their happy paths skip:
// arrays and booleans fall through to the defaults, arrays serialize through
// the string getter's fallback, and has() distinguishes stored keys.
BOOST_AUTO_TEST_CASE(TypedGettersOnCrossTypeKeys) {
    Config c;
    c.set("arr", ConfigValue(std::vector<std::string>{"one", "two"}));
    c.set("flag", ConfigValue(true));
    c.set("num", ConfigValue(int64_t{11}));
    c.set("real", ConfigValue(0.5));
    c.set("name", ConfigValue(std::string("val")));

    // Array-typed keys: every scalar getter falls back to its default.
    BOOST_CHECK_EQUAL(c.get_string("arr", "fallback"), "fallback");
    BOOST_CHECK_EQUAL(c.get_int("arr", -3), -3);
    BOOST_CHECK_EQUAL(c.get_double("arr", -0.5), -0.5);
    BOOST_CHECK_EQUAL(c.get_bool("arr", true), true);

    // Bool-typed keys through the numeric getters.
    BOOST_CHECK_EQUAL(c.get_int("flag", 9), 9);
    BOOST_CHECK_EQUAL(c.get_double("flag", 9.5), 9.5);

    // String-typed keys through the numeric/bool getters (parse fallbacks).
    BOOST_CHECK_EQUAL(c.get_int("name", 42), 42);
    BOOST_CHECK_EQUAL(c.get_double("name", 4.5), 4.5);

    // Array values still round-trip through the dedicated accessor.
    const auto items = c.get_string_array("arr");
    BOOST_REQUIRE_EQUAL(items.size(), 2U);
    BOOST_CHECK_EQUAL(items[0], "one");
    BOOST_CHECK_EQUAL(items[1], "two");
    // Non-array keys yield an empty vector.
    BOOST_CHECK(c.get_string_array("num").empty());
    BOOST_CHECK(c.get_string_array("missing").empty());

    // has() only reports stored keys.
    BOOST_CHECK(c.has("num"));
    BOOST_CHECK(!c.has("missing"));
}

// YAML scalars stored as strings exercise the conversion fallbacks of the
// typed getters when loaded (rather than set()).
BOOST_AUTO_TEST_CASE(TypedGettersOnLoadedUntaggedScalars) {
    Config c;
    BOOST_CHECK(c.load_yaml_string(
        "plain_int: 41\nplain_float: 0.25\nplain_bool: false\n"
        "listy: [a, b]\n"));
    // yaml-cpp 0.9 keeps plain scalars untagged, so they arrive as strings.
    BOOST_CHECK_EQUAL(c.get_int("plain_int"), 41);
    BOOST_CHECK_EQUAL(c.get_double("plain_float"), 0.25);
    BOOST_CHECK_EQUAL(c.get_bool("plain_bool"), false);
    // Sequences of scalars are stored as arrays.
    const auto items = c.get_string_array("listy");
    BOOST_REQUIRE_EQUAL(items.size(), 2U);
    BOOST_CHECK_EQUAL(items[0], "a");
    BOOST_CHECK_EQUAL(items[1], "b");
}

BOOST_AUTO_TEST_CASE(LoadYamlFromDirectoryPathFails) {
    Config c;
#if defined(__APPLE__)
    // macOS lets ifstream open a directory stream; the read yields an
    // empty document, so the loader reports success with no keys.
    BOOST_CHECK(c.load_yaml(kTmpDir.string()));
    BOOST_CHECK(!c.has("anything"));
#else
    // Opening a directory as a file fails; the loader reports false.
    BOOST_CHECK(!c.load_yaml(kTmpDir.string()));
    BOOST_CHECK(!c.has("anything"));
#endif
}

// Explicit YAML tags route scalars into the typed storage alternatives,
// and sequences containing non-scalars are dropped from the flat storage.
BOOST_AUTO_TEST_CASE(FlattenTagsAndNonScalarSequences) {
    Config c;
    BOOST_CHECK(
        c.load_yaml_string("ti: !!int 7\n"
                           "tf: !!float 2.5\n"
                           "tb: !!bool false\n"
                           "allsc: [x, y]\n"
                           "mixed: [a, [b]]\n"
                           "map_in_seq: [{k: 1}]\n"));
    BOOST_CHECK_EQUAL(c.get_int("ti"), 7);
    BOOST_CHECK_EQUAL(c.get_double("tf"), 2.5);
    BOOST_CHECK_EQUAL(c.get_bool("tb"), false);
    // Typed storage converts through the string getter.
    BOOST_CHECK_EQUAL(c.get_string("ti"), "7");
    BOOST_CHECK_EQUAL(c.get_string("tb"), "false");

    // All-scalar sequences survive flattening.
    BOOST_CHECK(c.has("allsc"));
    BOOST_REQUIRE_EQUAL(c.get_string_array("allsc").size(), 2U);
    // Sequences with nested structures are dropped instead of flattened.
    BOOST_CHECK(!c.has("mixed"));
    BOOST_CHECK(!c.has("map_in_seq"));
}

// Merging configs combines nested subtrees key by key (deep merge) while
// scalars from the overlay win.
BOOST_AUTO_TEST_CASE(MergeNestedYamlTrees) {
    Config a;
    BOOST_CHECK(a.load_yaml_string("x:\n  a: 1\nkeep: base\n"));
    Config b;
    BOOST_CHECK(b.load_yaml_string("x:\n  b: 2\nkeep: over\n"));
    a.merge(b);
    BOOST_CHECK_EQUAL(a.get_int("x.a"), 1);
    BOOST_CHECK_EQUAL(a.get_int("x.b"), 2);
    BOOST_CHECK_EQUAL(a.get_string("keep"), "over");
}

// Loading twice into the same Config deep-merges the YAML trees: nested maps
// union, overlay scalars replace.
BOOST_AUTO_TEST_CASE(ReloadDeepMergesNestedTrees) {
    Config c;
    BOOST_CHECK(
        c.load_yaml_string("x:\n  a: 1\n  inner:\n    i: 10\nkeep: base\n"));
    BOOST_CHECK(
        c.load_yaml_string("x:\n  b: 2\n  inner:\n    j: 20\nkeep: over\n"));
    BOOST_CHECK_EQUAL(c.get_int("x.a"), 1);
    BOOST_CHECK_EQUAL(c.get_int("x.b"), 2);
    BOOST_CHECK_EQUAL(c.get_int("x.inner.i"), 10);
    BOOST_CHECK_EQUAL(c.get_int("x.inner.j"), 20);
    BOOST_CHECK_EQUAL(c.get_string("keep"), "over");

    // Loading an empty document keeps the previous tree intact.
    BOOST_CHECK(c.load_yaml_string(""));
    BOOST_CHECK_EQUAL(c.get_int("x.a"), 1);
}

// ------------------------------------------------- extra coverage cases

// flatten_yaml_node records scalars with their YAML-resolved tag: int, float
// and bool values must land in storage as their typed ConfigValue variants
// (and be readable through the typed getters).
BOOST_AUTO_TEST_CASE(YamlScalarTypesFlattenAndFetch) {
    Config c;
    // Explicit tags make yaml-cpp report !!int / !!float / !!bool so the
    // flatten step stores typed variants instead of strings.
    BOOST_REQUIRE(c.load_yaml_string(
        "a:\n  i: !!int 42\n  f: !!float 3.5\n  b: !!bool true\n  "
        "bfalse: !!bool false\n  s: text\n"));
    BOOST_CHECK_EQUAL(c.get_int("a.i"), 42);
    BOOST_CHECK_EQUAL(c.get_double("a.f"), 3.5);
    BOOST_CHECK_EQUAL(c.get_bool("a.b"), true);
    BOOST_CHECK_EQUAL(c.get_bool("a.bfalse"), false);
    BOOST_CHECK_EQUAL(c.get_string("a.s"), "text");
}

// to_json walks nested maps (yaml_to_json object branch), including a
// nested-in-nested map, and stringifies scalar leaf values.
BOOST_AUTO_TEST_CASE(ToJsonWithNestedMaps) {
    Config c;
    BOOST_REQUIRE(c.load_yaml_string("l1:\n  l2:\n    leaf: 7\n  s: str\n"));
    // to_json flattens nested maps into dotted keys.
    const auto j = c.to_json();
    BOOST_CHECK(j.find("l1.l2.leaf") != std::string::npos);
    BOOST_CHECK(j.find("l1.s") != std::string::npos);
}

// set() stores every ConfigValue alternative; get_value() hands each back
// with the matching alternative active.
BOOST_AUTO_TEST_CASE(SetAndGetEveryConfigValueType) {
    Config c;

    c.set("v.str", ConfigValue{std::string("s")});
    c.set("v.i", ConfigValue{int64_t{-7}});
    c.set("v.d", ConfigValue{double{2.25}});
    c.set("v.b", ConfigValue{true});
    c.set("v.vec", ConfigValue{std::vector<std::string>{"x", "y"}});

    const auto* str = c.get_value("v.str");
    BOOST_REQUIRE(str != nullptr);
    BOOST_CHECK_EQUAL(std::get<std::string>(*str), "s");
    const auto* i = c.get_value("v.i");
    BOOST_REQUIRE(i != nullptr);
    BOOST_CHECK_EQUAL(std::get<int64_t>(*i), -7);
    const auto* d = c.get_value("v.d");
    BOOST_REQUIRE(d != nullptr);
    BOOST_CHECK_EQUAL(std::get<double>(*d), 2.25);
    const auto* b = c.get_value("v.b");
    BOOST_REQUIRE(b != nullptr);
    BOOST_CHECK_EQUAL(std::get<bool>(*b), true);
    const auto* vec = c.get_value("v.vec");
    BOOST_REQUIRE(vec != nullptr);
    const auto& as_vec = std::get<std::vector<std::string>>(*vec);
    BOOST_REQUIRE_EQUAL(as_vec.size(), 2u);
    BOOST_CHECK_EQUAL(as_vec[0], "x");
    BOOST_CHECK_EQUAL(as_vec[1], "y");

    // Unknown keys report absence.
    BOOST_CHECK(c.get_value("v.missing") == nullptr);
    BOOST_CHECK(!c.has("v.missing"));

    // get_string_array returns the stored vector via the typed accessor.
    const auto list = c.get_string_array("v.vec");
    BOOST_REQUIRE_EQUAL(list.size(), 2u);
    BOOST_CHECK_EQUAL(list[0], "x");
    BOOST_CHECK_EQUAL(list[1], "y");
}

// A tcp listener without a protocol block is a config error: raw-frame
// ingress was removed, so the session would have no inbound dispatch.
BOOST_AUTO_TEST_CASE(ValidateTcpWithoutProtocolIsRejected) {
    // Default options validate actors (require_actors=true), which is where
    // the per-actor network/protocol checks run.
    expect_invalid(actor_cfg("    network:\n      tcp: \"127.0.0.1:18112\"\n"),
                   RuntimeValidationOptions{}, "requires network.protocol");
}

// The same rejection through the checked-in fixture file.
BOOST_AUTO_TEST_CASE(TcpWithoutProtocolFixtureIsRejected) {
    shield::config::reset_config();
    auto& cfg = shield::config::global_config();
    BOOST_REQUIRE(
        cfg.load_yaml((fs::path(SHIELD_SOURCE_DIR) / "tests" / "fixtures" /
                       "config" / "tcp-without-protocol.yaml")
                          .string()));
    std::string error;
    const bool ok = shield::config::validate_runtime_config(
        RuntimeValidationOptions{}, &error);
    shield::config::reset_config();
    BOOST_CHECK(!ok);
    BOOST_CHECK_NE(error.find("requires network.protocol"), std::string::npos);
}

// protocol must be a mapping when present.
BOOST_AUTO_TEST_CASE(ValidateNetworkProtocolScalarRejected) {
    expect_invalid(
        actor_cfg("    network:\n      tcp: \"127.0.0.1:18113\"\n      "
                  "protocol: 42\n"),
        RuntimeValidationOptions{}, "protocol must be a map");
}

// ... and it must not be empty.
BOOST_AUTO_TEST_CASE(ValidateNetworkProtocolEmptyMapRejected) {
    expect_invalid(
        actor_cfg("    network:\n      tcp: \"127.0.0.1:18114\"\n      "
                  "protocol: {}\n"),
        RuntimeValidationOptions{}, "must not be empty");
}

// reload_config() is a stub that always reports success.
BOOST_AUTO_TEST_CASE(ReloadConfigReturnsTrue) {
    BOOST_CHECK(shield::config::reload_config());
}

// subtree_json with an empty path returns the empty-object document.
BOOST_AUTO_TEST_CASE(SubtreeJsonEmptyPathReturnsEmptyObject) {
    Config c;
    BOOST_REQUIRE(c.load_yaml_string("a:\n  b: 1\n"));
    BOOST_CHECK_EQUAL(shield::config::subtree_json(c, ""), "{}");
    // The nested map subtree serializes with its nested structure intact.
    const auto subtree = shield::config::subtree_json(c, "a");
    BOOST_CHECK_NE(subtree.find("\"b\":1"), std::string::npos);
    // Missing paths also produce the empty-object document.
    BOOST_CHECK_EQUAL(shield::config::subtree_json(c, "nope"), "{}");
}
