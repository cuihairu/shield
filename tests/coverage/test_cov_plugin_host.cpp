#define BOOST_TEST_MODULE CovPluginHost
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "shield/plugin/host_api.h"
#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/plugin_library.hpp"

extern "C" {
typedef struct lua_State lua_State;
lua_State* luaL_newstate(void);
void luaL_openselectedlibs(lua_State*, int, int);
void lua_close(lua_State*);
int lua_getglobal(lua_State*, const char*);
int lua_getfield(lua_State*, int, const char*);
const char* lua_tolstring(lua_State*, int, size_t*);
void lua_settop(lua_State*, int);
void lua_pushnil(lua_State*);
int lua_setglobal(lua_State*, const char*);
}

namespace fs = std::filesystem;
using namespace shield::plugin;

namespace {

struct FakeTestInterface {
    static constexpr const char* interface_name = "fake.test.iface";
    uint32_t struct_size;
    int marker;
};

const char* kFakePluginSource = R"FAKE(#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

#include <cstring>

namespace {

struct fake_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host;
    shield_plugin_context_v1* ctx;
    bool fail_start;
    bool fail_register = false;
};

const shield_host_api_v1* g_host_api = nullptr;
shield_plugin_context_v1* g_ctx_consumer = nullptr;
shield_plugin_context_v1* g_ctx_other = nullptr;
int g_add_path_rc = -99;
int g_add_cpath_rc = -99;
int g_lua_state_matches = -1;
int g_started = 0;
int g_shutdown_ok = 0;
int g_released_unstarted = 0;
const int g_iface_sentinel = 77;

const void* fake_get_iface(shield_plugin_instance_v1*, const char* iface,
                           shield_error_v1*) {
    if (iface && std::strcmp(iface, "fake.test.iface") == 0)
        return &g_iface_sentinel;
    return nullptr;
}

const void* null_iface(shield_plugin_instance_v1*, const char*,
                       shield_error_v1*) {
    return nullptr;
}

int fake_start(shield_plugin_instance_v1* self, shield_error_v1* err) {
    auto* i = reinterpret_cast<fake_instance*>(self);
    if (i && i->fail_start) {
        if (err) {
            err->code = "fake.start_failed";
            err->message = "requested start failure";
            err->phase = "start";
        }
        return -1;
    }
    ++g_started;
    return 0;
}

void fake_shutdown(shield_plugin_instance_v1* self) {
    ++g_shutdown_ok;
    delete reinterpret_cast<fake_instance*>(self);
}

void bump_released(shield_plugin_instance_v1*) { ++g_released_unstarted; }

int fake_register_lua(shield_plugin_instance_v1* self, struct lua_State* L,
                      struct shield_error_v1*) {
    auto* i = reinterpret_cast<fake_instance*>(self);
    if (!i || !i->host) return 0;
    if (i->fail_register) return -1;
    g_lua_state_matches = (i->host->lua_state(i->ctx) == L) ? 1 : 0;
    g_add_path_rc = i->host->lua_add_path(i->ctx, "lua/?.lua", 0);
    g_add_cpath_rc = i->host->lua_add_path(i->ctx, "lua/?.so", 1);
    return 0;
}

int fake_create(const struct shield_plugin_create_args_v1* args,
                struct shield_plugin_instance_v1** out,
                struct shield_error_v1* err) {
    if (!args || !out) {
        if (err) {
            err->code = "fake.args";
            err->message = "missing create args";
        }
        return -1;
    }
    g_host_api = args->host_api;
    const char* cfg = args->config_json ? args->config_json : "{}";
    if (std::strstr(cfg, "\"create_fail\"")) {
        if (err) {
            err->code = "fake.create_failed";
            err->message = "requested create failure";
            err->phase = "create";
            err->instance_id = args->instance_id;
        }
        *out = nullptr;
        return -1;
    }
    if (std::strstr(cfg, "\"bad_handle\"") ||
        std::strstr(cfg, "\"no_getif\"") || std::strstr(cfg, "\"no_iface\"")) {
        static struct shield_plugin_instance_v1 shell;
        shell.struct_size = std::strstr(cfg, "\"bad_handle\"")
                                ? 8u
                                : (uint32_t)sizeof(shield_plugin_instance_v1);
        shell.instance_id = args->instance_id;
        shell.get_interface =
            std::strstr(cfg, "\"no_iface\"") ? null_iface : nullptr;
        shell.start = nullptr;
        shell.shutdown = bump_released;
        shell.register_lua = nullptr;
        *out = &shell;
        return 0;
    }
    auto* inst = new fake_instance;
    inst->host = args->host_api;
    inst->ctx = args->ctx;
    inst->fail_start = std::strstr(cfg, "\"start_fail\"") != nullptr;
    inst->fail_register = std::strstr(cfg, "\"register_fail\"") != nullptr;
    inst->shell.struct_size = (uint32_t)sizeof(fake_instance);
    inst->shell.instance_id = args->instance_id;
    inst->shell.get_interface = fake_get_iface;
    inst->shell.start = fake_start;
    inst->shell.shutdown = fake_shutdown;
    inst->shell.register_lua = fake_register_lua;
    if (args->instance_id && std::strstr(args->instance_id, "consumer"))
        g_ctx_consumer = args->ctx;
    else
        g_ctx_other = args->ctx;
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_host_api_v1*
fake_host_api(void) {
    return g_host_api;
}
extern "C" SHIELD_PLUGIN_EXPORT struct shield_plugin_context_v1*
fake_ctx_consumer(void) {
    return g_ctx_consumer;
}
extern "C" SHIELD_PLUGIN_EXPORT struct shield_plugin_context_v1*
fake_ctx_other(void) {
    return g_ctx_other;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_add_path_rc(void) {
    return g_add_path_rc;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_add_cpath_rc(void) {
    return g_add_cpath_rc;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_lua_state_matches(void) {
    return g_lua_state_matches;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_started_count(void) {
    return g_started;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_shutdown_count(void) {
    return g_shutdown_ok;
}
extern "C" SHIELD_PLUGIN_EXPORT int fake_released_unstarted(void) {
    return g_released_unstarted;
}
extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
fake_entry_ok(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        (uint32_t)sizeof(struct shield_plugin_abi_v1), "fake.test", "9.9.9",
        fake_create};
    return &abi;
}
extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
fake_entry_null(void) {
    return nullptr;
}
extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
fake_entry_badstruct(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION, 4, "fake.test", "9.9.9", fake_create};
    return &abi;
}
)FAKE";

fs::path source_include_dir() {
    fs::path p = fs::path(__FILE__).parent_path();
    return (p / ".." / ".." / "include").lexically_normal();
}

fs::path unique_root(const std::string& tag) {
    auto root = fs::temp_directory_path() / "shield_cov_plugin_host" / tag;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

struct FakePlugin {
    fs::path so;
    bool ok = false;
    FakePlugin() {
        auto dir = fs::temp_directory_path() / "shield_cov_plugin_host" /
                   "fake_plugin";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        auto src = dir / "fake_plugin.cpp";
        write_file(src, kFakePluginSource);
        so = dir / "libfake_multi_test_plugin.so";
        std::vector<std::string> compilers;
        if (const char* cxx = std::getenv("CXX")) compilers.push_back(cxx);
        compilers.push_back("/usr/bin/x86_64-linux-gnu-g++-15");
        compilers.push_back("/usr/bin/g++");
        compilers.push_back("/usr/bin/c++");
        const auto inc = source_include_dir().string();
        for (const auto& c : compilers) {
            std::ostringstream cmd;
            cmd << c << " -std=c++17 -shared -fPIC -I\"" << inc << "\" -o \""
                << so.string() << "\" \"" << src.string() << "\" 2>\""
                << (dir / "build.log").string() << "\"";
            if (std::system(cmd.str().c_str()) == 0 && fs::exists(so)) {
                ok = true;
                break;
            }
        }
    }
};

FakePlugin& fake_plugin() {
    static FakePlugin p;
    return p;
}

fs::path minimal_test_so() {
#if defined(_WIN32)
    return fs::path(
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.dll");
#elif defined(__APPLE__)
    return fs::path(
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.dylib");
#else
    return fs::path(
        "test_plugins/minimal.test/bin/"
        "libshield_minimal_test_plugin.so");
#endif
}

std::string fake_manifest(const std::string& id,
                          const std::string& entry = "fake_entry_ok",
                          const std::string& provides = "fake.test.iface",
                          const std::string& requires_yaml = "",
                          const std::string& extra = "",
                          const std::string& linux_lib = "bin/libfake.so") {
    std::ostringstream o;
    o << "schema_version: 1\n"
      << "id: " << id << "\n"
      << "name: " << id << "\n"
      << "version: 1.0.0\n"
      << "kind: coverage\n"
      << "entry: " << entry << "\n"
      << "library:\n"
      << "  linux: " << linux_lib << "\n"
      << "  macos: " << linux_lib << "\n"
      << "  windows: bin/other.dll\n";
    if (provides.empty())
        o << "provides: []\n";
    else
        o << "provides:\n  - interface: " << provides << "\n";
    if (requires_yaml.empty())
        o << "requires: []\n";
    else
        o << "requires:\n" << requires_yaml;
    o << "config_schema:\n  type: object\n";
    o << extra;
    return o.str();
}

fs::path make_package(const fs::path& root, const std::string& dir,
                      const std::string& manifest_content, bool with_fake_so,
                      const fs::path& so_file = {},
                      const std::string& so_name = "libfake.so") {
    auto pkg = root / dir;
    fs::create_directories(pkg / "bin");
    write_file(pkg / "manifest.yaml", manifest_content);
    if (with_fake_so)
        fs::copy_file(fake_plugin().so, pkg / "bin" / so_name,
                      fs::copy_options::overwrite_existing);
    else if (!so_file.empty())
        fs::copy_file(so_file, pkg / "bin" / so_name,
                      fs::copy_options::overwrite_existing);
    return pkg;
}

InstanceDecl decl(const std::string& id, const std::string& pkg,
                  bool required = true,
                  std::map<std::string, std::string> deps = {},
                  nlohmann::json config = nlohmann::json::object()) {
    InstanceDecl d;
    d.id = id;
    d.package = pkg;
    d.required = required;
    d.dependencies = std::move(deps);
    d.config = std::move(config);
    return d;
}

BindingDecl binding(const std::string& logical, const std::string& inst) {
    BindingDecl b;
    b.logical = logical;
    b.instance_id = inst;
    return b;
}

struct FakeSymbols {
    PluginLibrary lib;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* (*ctx_consumer)() = nullptr;
    shield_plugin_context_v1* (*ctx_other)() = nullptr;
    int (*add_path_rc)() = nullptr;
    int (*add_cpath_rc)() = nullptr;
    int (*lua_state_matches)() = nullptr;
    int (*released_unstarted)() = nullptr;
};

FakeSymbols load_fake_symbols(const fs::path& so) {
    FakeSymbols s;
    std::string err;
    s.lib = PluginLibrary::load(so.string(), err);
    BOOST_REQUIRE_MESSAGE(s.lib.is_loaded(), err);
    s.host_api = reinterpret_cast<const shield_host_api_v1* (*)()>(
        s.lib.resolve("fake_host_api"))();
    s.ctx_consumer = reinterpret_cast<shield_plugin_context_v1* (*)()>(
        s.lib.resolve("fake_ctx_consumer"));
    s.ctx_other = reinterpret_cast<shield_plugin_context_v1* (*)()>(
        s.lib.resolve("fake_ctx_other"));
    s.add_path_rc =
        reinterpret_cast<int (*)()>(s.lib.resolve("fake_add_path_rc"));
    s.add_cpath_rc =
        reinterpret_cast<int (*)()>(s.lib.resolve("fake_add_cpath_rc"));
    s.lua_state_matches =
        reinterpret_cast<int (*)()>(s.lib.resolve("fake_lua_state_matches"));
    s.released_unstarted =
        reinterpret_cast<int (*)()>(s.lib.resolve("fake_released_unstarted"));
    BOOST_REQUIRE(s.host_api);
    BOOST_REQUIRE(s.ctx_consumer);
    BOOST_REQUIRE(s.ctx_other);
    BOOST_REQUIRE(s.add_path_rc);
    BOOST_REQUIRE(s.add_cpath_rc);
    BOOST_REQUIRE(s.lua_state_matches);
    BOOST_REQUIRE(s.released_unstarted);
    return s;
}

struct LuaDeleter {
    void operator()(lua_State* L) const {
        if (L) lua_close(L);
    }
};

std::unique_ptr<lua_State, LuaDeleter> make_lua() {
    auto* L = luaL_newstate();
    if (L) luaL_openselectedlibs(L, ~0, 0);
    return std::unique_ptr<lua_State, LuaDeleter>(L);
}

std::string lua_package_field(lua_State* L, const char* field) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, field);
    size_t n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    std::string r = s ? std::string(s, n) : std::string();
    lua_settop(L, 0);
    return r;
}

std::string run_catalog(const std::string& manifest_content,
                        const std::string& tag) {
    auto root = unique_root(tag);
    write_file(root / "pkg" / "manifest.yaml", manifest_content);
    PluginHost host;
    host.scan(root.string());
    std::string err = "no-error";
    host.catalog(err);
    fs::remove_all(root);
    return err;
}

bool fake_ready() {
    auto& fp = fake_plugin();
    if (!fp.ok) BOOST_TEST_MESSAGE("fake plugin compile unavailable; skipping");
    return fp.ok;
}

}  // namespace

BOOST_AUTO_TEST_CASE(scan_skips_unparseable_manifest) {
    auto root = unique_root("scan_bad_manifest");
    write_file(root / "broken" / "manifest.yaml",
               "schema_version: 1\nid: [unclosed\nlibrary:\n");
    write_file(root / "good" / "manifest.yaml", fake_manifest("good.test"));
    PluginHost host;
    host.scan("/nonexistent/shield/cov/dir");
    host.scan(root.string());
    auto ids = host.package_ids();
    BOOST_REQUIRE_EQUAL(ids.size(), 1u);
    BOOST_CHECK_EQUAL(ids[0], "good.test");
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(catalog_rejects_manifest_variants) {
    BOOST_TEST(run_catalog("schema_version: 1\n"
                           "id: nolibpath.test\n"
                           "entry: fake_entry_ok\n"
                           "library:\n"
                           "  windows: bin/x.dll\n"
                           "provides:\n"
                           "  - interface: fake.test.iface\n"
                           "requires: []\n"
                           "config_schema:\n"
                           "  type: object\n",
                           "cat_nolibpath")
                   .find("missing library path") != std::string::npos);
    BOOST_TEST(run_catalog("schema_version: 1\n"
                           "id: emptyiface.test\n"
                           "entry: fake_entry_ok\n"
                           "library:\n"
                           "  linux: bin/libfake.so\n"
                           "provides:\n"
                           "  - interface: \"\"\n"
                           "requires: []\n"
                           "config_schema:\n"
                           "  type: object\n",
                           "cat_emptyiface")
                   .find("empty provided interface") != std::string::npos);
    BOOST_TEST(
        run_catalog(
            fake_manifest("baddep.test", "fake_entry_ok", "fake.test.iface",
                          "  - name: \"\"\n    interface: x.iface\n"),
            "cat_baddep")
            .find("invalid dependency") != std::string::npos);
    BOOST_TEST(
        run_catalog(
            fake_manifest("dupdep.test", "fake_entry_ok", "fake.test.iface",
                          "  - name: dep\n    interface: x.iface\n"
                          "  - name: dep\n    interface: y.iface\n"),
            "cat_dupdep")
            .find("duplicate dependency") != std::string::npos);
    BOOST_TEST(run_catalog(fake_manifest("abslib.test", "fake_entry_ok",
                                         "fake.test.iface", "", "",
                                         "/absolute/libfake.so"),
                           "cat_abslib")
                   .find("package root") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(plan_rejects_empty_and_duplicate_instance_ids) {
    auto root = unique_root("plan_ids");
    write_file(root / "plain" / "manifest.yaml", fake_manifest("plain.test"));
    PluginHost host;
    std::string err;
    host.scan(root.string());
    BOOST_REQUIRE(host.catalog(err));

    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("", "plain.test"));
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("instance id is required") != std::string::npos);

    PluginConfig cfg2;
    cfg2.directory = root.string();
    cfg2.instances.push_back(decl("x", "plain.test"));
    cfg2.instances.push_back(decl("x", "plain.test"));
    BOOST_CHECK(!host.plan_and_resolve(cfg2, err));
    BOOST_TEST(err.find("duplicate instance id") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(plan_dependency_resolution_errors) {
    auto root = unique_root("plan_deps");
    write_file(root / "plain" / "manifest.yaml",
               fake_manifest("plain.test", "fake_entry_ok", "iface.plain"));
    write_file(root / "strict" / "manifest.yaml",
               fake_manifest("strict.test", "fake_entry_ok", "iface.strict",
                             "  - name: dep\n    interface: iface.plain\n"));
    write_file(root / "picky" / "manifest.yaml",
               fake_manifest("picky.test", "fake_entry_ok", "iface.picky",
                             "  - name: dep\n    interface: iface.other\n"));
    write_file(root / "pickyopt" / "manifest.yaml",
               fake_manifest("pickyopt.test", "fake_entry_ok", "iface.pickyopt",
                             "  - name: dep\n    interface: iface.other\n"
                             "    optional: true\n"));
    PluginHost host;
    std::string err;
    host.scan(root.string());
    BOOST_REQUIRE(host.catalog(err));

    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(
            decl("u", "plain.test", true, {{"ghost", "g"}}));
        BOOST_CHECK(!host.plan_and_resolve(cfg, err));
        BOOST_TEST(err.find("plugin.dependency.undeclared") !=
                   std::string::npos);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(
            decl("u", "plain.test", false, {{"ghost", "g"}}));
        err.clear();
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        const Instance* inst = host.find_instance("u");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m1", "strict.test", true, {}));
        BOOST_CHECK(!host.plan_and_resolve(cfg, err));
        BOOST_TEST(err.find("missing required dependency") !=
                   std::string::npos);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m2", "strict.test", false, {}));
        err.clear();
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        const Instance* inst = host.find_instance("m2");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(
            decl("m3", "strict.test", true, {{"dep", "ghost"}}));
        BOOST_CHECK(!host.plan_and_resolve(cfg, err));
        BOOST_TEST(err.find("not found") != std::string::npos);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(
            decl("m4", "strict.test", false, {{"dep", "ghost"}}));
        err.clear();
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        const Instance* inst = host.find_instance("m4");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("p", "plain.test"));
        cfg.instances.push_back(decl("c", "picky.test", true, {{"dep", "p"}}));
        BOOST_CHECK(!host.plan_and_resolve(cfg, err));
        BOOST_TEST(err.find("does not provide") != std::string::npos);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("p", "plain.test"));
        cfg.instances.push_back(
            decl("c2", "picky.test", false, {{"dep", "p"}}));
        err.clear();
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        const Instance* inst = host.find_instance("c2");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    {
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("p", "plain.test"));
        cfg.instances.push_back(
            decl("po", "pickyopt.test", true, {{"dep", "p"}}));
        err.clear();
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        const Instance* inst = host.find_instance("po");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::planned);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(plan_rejects_invalid_bindings) {
    auto root = unique_root("plan_bindings");
    write_file(root / "plain" / "manifest.yaml", fake_manifest("plain.test"));
    PluginHost host;
    std::string err;
    host.scan(root.string());
    BOOST_REQUIRE(host.catalog(err));

    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("m", "plain.test"));
    cfg.bindings.push_back(binding("", "m"));
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("binding name is required") != std::string::npos);

    PluginConfig cfg2;
    cfg2.directory = root.string();
    cfg2.instances.push_back(decl("m", "plain.test"));
    cfg2.bindings.push_back(binding("b", "m"));
    cfg2.bindings.push_back(binding("b", "m"));
    BOOST_CHECK(!host.plan_and_resolve(cfg2, err));
    BOOST_TEST(err.find("duplicate binding") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(load_failures_required) {
    if (!fake_ready()) return;
    auto root = unique_root("load_required");
    make_package(root, "nolib", fake_manifest("nolib.test"), false);
    make_package(root, "entrymiss",
                 fake_manifest("entrymiss.test", "no_such_entry"), true);
    make_package(root, "nullabi",
                 fake_manifest("nullabi.test", "fake_entry_null"), true);
    make_package(root, "badstruct",
                 fake_manifest("badstruct.test", "fake_entry_badstruct"), true);
    make_package(root, "wrongid", fake_manifest("wrongid.test"), true);

    struct Case {
        const char* pkg;
        const char* expect;
    };
    const Case cases[] = {
        {"nolib.test", "plugin.entry.missing"},
        {"entrymiss.test", "not found in"},
        {"nullabi.test", "(abi_version)"},
        {"badstruct.test", "(struct_size)"},
        {"wrongid.test", "(package_id"},
    };
    for (const auto& c : cases) {
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("r", c.pkg, true));
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find(c.expect) != std::string::npos);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(load_failures_optional) {
    if (!fake_ready()) return;
    auto root = unique_root("load_optional");
    make_package(root, "entrymiss",
                 fake_manifest("entrymiss.test", "no_such_entry"), true);
    make_package(root, "nullabi",
                 fake_manifest("nullabi.test", "fake_entry_null"), true);
    make_package(root, "badstruct",
                 fake_manifest("badstruct.test", "fake_entry_badstruct"), true);
    make_package(root, "wrongid", fake_manifest("wrongid.test"), true);

    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("o1", "entrymiss.test", false));
    cfg.instances.push_back(decl("o2", "nullabi.test", false));
    cfg.instances.push_back(decl("o3", "badstruct.test", false));
    cfg.instances.push_back(decl("o4", "wrongid.test", false));
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    for (const char* id : {"o1", "o2", "o3", "o4"}) {
        const Instance* inst = host.find_instance(id);
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(create_failures_optional) {
    if (!fake_ready()) return;
    auto root = unique_root("create_optional");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("c1", "fake.test", false, {},
                                 nlohmann::json{{"mode", "create_fail"}}));
    cfg.instances.push_back(decl("c2", "fake.test", false, {},
                                 nlohmann::json{{"mode", "bad_handle"}}));
    cfg.instances.push_back(decl("c3", "fake.test", false, {},
                                 nlohmann::json{{"mode", "no_getif"}}));
    cfg.instances.push_back(decl("c4", "fake.test", false, {},
                                 nlohmann::json{{"mode", "no_iface"}}));
    cfg.instances.push_back(decl("c5", "fake.test", false, {},
                                 nlohmann::json{{"mode", "start_fail"}}));
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    for (const char* id : {"c1", "c2", "c3", "c4", "c5"}) {
        const Instance* inst = host.find_instance(id);
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    {
        auto symbols =
            load_fake_symbols(root / "fake.test" / "bin" / "libfake.so");
        BOOST_CHECK_EQUAL(symbols.released_unstarted(), 3);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(create_failures_required) {
    if (!fake_ready()) return;
    auto root = unique_root("create_required");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    struct Case {
        const char* mode;
        const char* expect;
    };
    const Case cases[] = {
        {"create_fail", "plugin.create.failed"},
        {"bad_handle", "(instance struct_size)"},
        {"no_getif", "missing get_interface"},
        {"no_iface", "does not provide declared interface"},
    };
    for (const auto& c : cases) {
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(
            decl("r", "fake.test", true, {}, nlohmann::json{{"mode", c.mode}}));
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find(c.expect) != std::string::npos);
        const Instance* inst = host.find_instance("r");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::failed);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(start_blocked_required_consumer) {
    if (!fake_ready()) return;
    auto root = unique_root("start_blocked_req");
    make_package(
        root, "provider",
        fake_manifest("provider.test", "fake_entry_ok", "fake.test.iface"),
        false);
    make_package(
        root, "fake.test",
        fake_manifest("fake.test", "fake_entry_ok", "fake.test.iface",
                      "  - name: dep\n    interface: fake.test.iface\n"),
        true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("prov", "provider.test", false));
    cfg.instances.push_back(
        decl("consumer", "fake.test", true, {{"dep", "prov"}}));
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_TEST(err.find("plugin.dependency.unavailable") != std::string::npos);
    const Instance* consumer = host.find_instance("consumer");
    BOOST_REQUIRE(consumer);
    BOOST_CHECK(consumer->state == State::failed);
    const Instance* prov = host.find_instance("prov");
    BOOST_REQUIRE(prov);
    BOOST_CHECK(prov->state == State::unavailable);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(optional_consumer_blocked_and_api_probes) {
    if (!fake_ready()) return;
    auto root = unique_root("start_blocked_opt");
    make_package(
        root, "provider",
        fake_manifest("provider.test", "fake_entry_ok", "fake.test.iface"),
        false);
    make_package(
        root, "fake.test",
        fake_manifest("fake.test", "fake_entry_ok", "fake.test.iface",
                      "  - name: dep\n    interface: fake.test.iface\n"),
        true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("prov", "provider.test", false));
    cfg.instances.push_back(
        decl("consumer", "fake.test", false, {{"dep", "prov"}},
             nlohmann::json{{"mode", "default"}, {"name", "n"}}));
    cfg.bindings.push_back(binding("b1", "consumer"));
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    const Instance* consumer = host.find_instance("consumer");
    BOOST_REQUIRE(consumer);
    BOOST_CHECK(consumer->state == State::unavailable);

    auto symbols = load_fake_symbols(root / "fake.test" / "bin" / "libfake.so");
    auto* api = symbols.host_api;
    auto* ctx = symbols.ctx_consumer();
    BOOST_REQUIRE(ctx);

    BOOST_CHECK(api->dependency(nullptr, "dep", "fake.test.iface") == nullptr);
    BOOST_CHECK(api->dependency(ctx, nullptr, nullptr) == nullptr);
    BOOST_CHECK(api->dependency(ctx, "ghost", "fake.test.iface") == nullptr);
    BOOST_CHECK(api->dependency(ctx, "dep", "zzz.iface") == nullptr);
    BOOST_CHECK(api->dependency(ctx, "dep", "fake.test.iface") == nullptr);

    BOOST_CHECK(host.get_by_binding<FakeTestInterface>("b1") == nullptr);
    BOOST_CHECK(host.get_by_binding<FakeTestInterface>("missing") == nullptr);
    BOOST_CHECK_EQUAL(host.binding_instance_id("b1"), "consumer");
    BOOST_CHECK_EQUAL(host.binding_instance_id("nope"), "");

    auto info = host.get_binding("b1");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(info->logical, "b1");
    BOOST_CHECK_EQUAL(info->instance_id, "consumer");
    BOOST_CHECK_EQUAL(info->interface_name, "fake.test.iface");
    BOOST_CHECK(!host.get_binding("nope").has_value());

    BOOST_CHECK(api->config_get(nullptr, "mode") == nullptr);
    BOOST_CHECK(api->config_get(ctx, nullptr) == nullptr);
    BOOST_CHECK_EQUAL(api->config_get(ctx, "mode"), "default");
    BOOST_CHECK_EQUAL(api->config_get(ctx, "name"), "n");
    BOOST_CHECK(api->config_get(ctx, "missing") == nullptr);
    BOOST_CHECK(api->config_get(ctx, "missing.deep") == nullptr);

    BOOST_CHECK_EQUAL(api->lua_add_path(nullptr, "x", 0), -1);
    BOOST_CHECK_EQUAL(api->lua_add_path(ctx, nullptr, 0), -1);
    BOOST_CHECK_EQUAL(api->lua_add_path(ctx, "lua/?.lua", 0), -1);

    BOOST_CHECK(api->binding_instance_id(ctx, nullptr) == nullptr);
    BOOST_CHECK(api->binding_instance_id(ctx, "") == nullptr);
    BOOST_CHECK_EQUAL(api->binding_instance_id(ctx, "b1"), "consumer");
    BOOST_CHECK(api->binding_instance_id(nullptr, "zzz") == nullptr);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(chain_success_host_api_battery) {
    if (!fake_ready()) return;
    BOOST_REQUIRE(fs::exists(minimal_test_so()));
    auto root = unique_root("chain_ok");
    make_package(root, "minimal.test",
                 fake_manifest("minimal.test", "shield_plugin_get_v1",
                               "minimal.test.iface", "", "",
                               "bin/libshield_minimal_test_plugin.so"),
                 false, minimal_test_so(), "libshield_minimal_test_plugin.so");
    make_package(
        root, "fake.test",
        fake_manifest("fake.test", "fake_entry_ok", "fake.test.iface",
                      "  - name: dep\n    interface: minimal.test.iface\n",
                      "documentation:\n"
                      "  url: https://example.invalid/fake\n"
                      "  description: fake package docs\n"),
        true);

    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("db", "minimal.test"));
    cfg.instances.push_back(
        decl("consumer", "fake.test", true, {{"dep", "db"}},
             nlohmann::json{{"mode", "default"},
                            {"name", "n"},
                            {"nested", nlohmann::json{{"port", 5}}}}));
    cfg.bindings.push_back(binding("b2", "consumer"));
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    BOOST_CHECK(host.find_instance("consumer")->state == State::started);
    BOOST_CHECK(host.find_instance("db")->state == State::started);

    auto symbols = load_fake_symbols(root / "fake.test" / "bin" / "libfake.so");
    auto* api = symbols.host_api;
    auto* ctx = symbols.ctx_consumer();
    BOOST_REQUIRE(ctx);

    BOOST_CHECK(api->dependency(ctx, "dep", "minimal.test.iface") != nullptr);
    BOOST_CHECK(api->dependency(ctx, "dep", "fake.test.iface") == nullptr);

    BOOST_CHECK_EQUAL(api->config_get(ctx, "nested.port"), "5");
    BOOST_CHECK(api->config_get(ctx, "nested") != nullptr);

    api->log(SHIELD_LOG_DEBUG, "fake.test", "consumer", "debug msg");
    api->log(SHIELD_LOG_INFO, "fake.test", "consumer", "info msg");
    api->log(SHIELD_LOG_WARN, "fake.test", "consumer", "warn msg");
    api->log(SHIELD_LOG_ERROR, "fake.test", "consumer", "error msg");
    api->log(SHIELD_LOG_INFO, nullptr, nullptr, nullptr);

    api->report_error(nullptr);
    shield_error_v1 full{};
    full.package_id = "fake.test";
    full.code = "fake.code";
    full.message = "boom";
    full.instance_id = "consumer";
    full.phase = "create";
    api->report_error(&full);
    shield_error_v1 empty_err{};
    api->report_error(&empty_err);

    BOOST_CHECK(api->lua_state(nullptr) == nullptr);

    auto pkgs = host.list_packages();
    BOOST_REQUIRE_EQUAL(pkgs.size(), 2u);
    for (const auto& p : pkgs) {
        if (p.id == "fake.test") {
            BOOST_CHECK_EQUAL(p.docs_url, "https://example.invalid/fake");
            BOOST_CHECK_EQUAL(p.docs_description, "fake package docs");
            BOOST_REQUIRE_EQUAL(p.provides.size(), 1u);
            BOOST_CHECK_EQUAL(p.provides[0], "fake.test.iface");
        }
    }
    bool saw_started = false;
    for (const auto& i : host.list_instances()) {
        if (i.id == "consumer") {
            BOOST_CHECK_EQUAL(i.state, "started");
            BOOST_CHECK(i.required);
            saw_started = true;
        }
    }
    BOOST_CHECK(saw_started);
    BOOST_CHECK(host.find_package("nope") == nullptr);
    BOOST_CHECK(&global_host() == &global_host());

    PluginHost hook_host;
    BOOST_CHECK(api->lua_current_service_id(nullptr) == nullptr);
    BOOST_CHECK_EQUAL(
        api->lua_post_to_service(nullptr, nullptr, nullptr, nullptr, nullptr),
        -1);
    BOOST_CHECK_EQUAL(
        api->lua_post_to_service(nullptr, "", nullptr, nullptr, nullptr), -1);
    BOOST_CHECK_EQUAL(
        api->lua_post_to_service(nullptr, "svc.a", nullptr, nullptr, nullptr),
        -1);

    LuaServiceHooks current_only;
    current_only.current_service_id = [] { return std::string("svc.a"); };
    hook_host.set_lua_service_hooks(current_only);
    BOOST_CHECK_EQUAL(api->lua_current_service_id(nullptr), "svc.a");

    struct PostSink {
        std::vector<std::function<void()>> tasks;
        uint64_t next_id = 1;
        bool fail_next = false;
        uint64_t post(const std::string&, std::function<void()> t) {
            if (fail_next) return 0;
            tasks.push_back(std::move(t));
            return next_id++;
        }
    };
    PostSink sink;
    struct PostCounters {
        int fn_calls = 0;
        int destroy_calls = 0;
    };
    PostCounters pc;
    void (*fn)(void*) = [](void* p) {
        ++static_cast<PostCounters*>(p)->fn_calls;
    };
    void (*dfn)(void*) = [](void* p) {
        ++static_cast<PostCounters*>(p)->destroy_calls;
    };

    LuaServiceHooks full_hooks;
    full_hooks.current_service_id = [] { return std::string("svc.a"); };
    full_hooks.post_to_service = [&sink](const std::string& s,
                                         std::function<void()> t) {
        return sink.post(s, std::move(t));
    };
    hook_host.set_lua_service_hooks(full_hooks);
    BOOST_CHECK_EQUAL(api->lua_current_service_id(nullptr), "svc.a");

    BOOST_CHECK_EQUAL(api->lua_post_to_service(nullptr, "svc.a", fn, &pc, dfn),
                      0);
    BOOST_REQUIRE_EQUAL(sink.tasks.size(), 1u);
    sink.tasks[0]();
    sink.tasks.clear();
    BOOST_CHECK_EQUAL(pc.fn_calls, 1);
    BOOST_CHECK_EQUAL(pc.destroy_calls, 2);

    BOOST_CHECK_EQUAL(api->lua_post_to_service(nullptr, "svc.a", fn, &pc, dfn),
                      0);
    sink.tasks.clear();
    BOOST_CHECK_EQUAL(pc.fn_calls, 1);
    BOOST_CHECK_EQUAL(pc.destroy_calls, 4);

    sink.fail_next = true;
    BOOST_CHECK_EQUAL(api->lua_post_to_service(nullptr, "svc.a", fn, &pc, dfn),
                      -1);
    BOOST_CHECK_EQUAL(pc.fn_calls, 1);
    BOOST_CHECK_EQUAL(pc.destroy_calls, 5);

    LuaServiceHooks empty_id_hooks;
    empty_id_hooks.current_service_id = [] { return std::string(); };
    hook_host.set_lua_service_hooks(empty_id_hooks);
    BOOST_CHECK(api->lua_current_service_id(nullptr) == nullptr);
    hook_host.set_lua_service_hooks(LuaServiceHooks{});
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(lua_inject_and_register_paths) {
    if (!fake_ready()) return;
    auto root = unique_root("lua_paths");
    make_package(
        root, "fake.test",
        fake_manifest("fake.test", "fake_entry_ok", "fake.test.iface", "",
                      "lua:\n"
                      "  namespace: fake\n"
                      "  search_paths:\n"
                      "    - lua/?.lua\n"
                      "    - \"\"\n"),
        true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("luainst", "fake.test"));
    cfg.instances.push_back(decl("failer", "fake.test", false, {},
                                 nlohmann::json{{"mode", "start_fail"}}));
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    BOOST_CHECK(host.find_instance("luainst")->state == State::started);
    BOOST_CHECK(host.find_instance("failer")->state == State::unavailable);

    host.inject_lua_paths(nullptr);

    auto abs_pattern =
        (root / "fake.test" / "lua" / "?.lua").lexically_normal().string();
    auto L1 = make_lua();
    BOOST_REQUIRE(L1);
    host.inject_lua_paths(L1.get());
    auto path1 = lua_package_field(L1.get(), "path");
    BOOST_TEST(path1.find(abs_pattern) != std::string::npos);

    auto L2 = make_lua();
    BOOST_REQUIRE(L2);
    lua_pushnil(L2.get());
    lua_setglobal(L2.get(), "package");
    host.inject_lua_paths(L2.get());
    BOOST_CHECK(host.register_lua_all(nullptr, err));

    auto symbols = load_fake_symbols(root / "fake.test" / "bin" / "libfake.so");
    BOOST_REQUIRE(host.register_lua_all(L2.get(), err));
    BOOST_CHECK_EQUAL(symbols.add_path_rc(), -1);
    BOOST_CHECK_EQUAL(symbols.add_cpath_rc(), -1);
    BOOST_CHECK_EQUAL(symbols.lua_state_matches(), 1);

    BOOST_REQUIRE(host.register_lua_all(L1.get(), err));
    BOOST_CHECK_EQUAL(symbols.add_path_rc(), 0);
    BOOST_CHECK_EQUAL(symbols.add_cpath_rc(), 0);
    BOOST_CHECK_EQUAL(symbols.lua_state_matches(), 1);
    auto cpath1 = lua_package_field(L1.get(), "cpath");
    auto abs_cpattern =
        (root / "fake.test" / "lua" / "?.so").lexically_normal().string();
    BOOST_TEST(cpath1.find(abs_cpattern) != std::string::npos);
    auto path_after = lua_package_field(L1.get(), "path");
    BOOST_TEST(path_after.find(abs_pattern) != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(instance_states_via_introspection) {
    if (!fake_ready()) return;
    {
        auto root = unique_root("state_planned");
        write_file(root / "plain" / "manifest.yaml",
                   fake_manifest("plain.test"));
        PluginHost host;
        std::string err;
        host.scan(root.string());
        BOOST_REQUIRE(host.catalog(err));
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "plain.test"));
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        auto infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "planned");
        BOOST_CHECK(infos[0].required);
        fs::remove_all(root);
    }
    {
        auto root = unique_root("state_loaded");
        make_package(root, "fake.test", fake_manifest("fake.test"), true);
        PluginHost host;
        std::string err;
        host.scan(root.string());
        BOOST_REQUIRE(host.catalog(err));
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "fake.test"));
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        BOOST_REQUIRE(host.load_all(err));
        auto infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "loaded");
        fs::remove_all(root);
    }
    {
        auto root = unique_root("state_unavailable");
        write_file(root / "plain" / "manifest.yaml",
                   fake_manifest("plain.test"));
        PluginHost host;
        std::string err;
        host.scan(root.string());
        BOOST_REQUIRE(host.catalog(err));
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "no.such.package", false));
        BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
        auto infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "unavailable");
        fs::remove_all(root);
    }
    {
        auto root = unique_root("state_failed");
        make_package(root, "fake.test", fake_manifest("fake.test"), true);
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "fake.test", true, {},
                                     nlohmann::json{{"mode", "create_fail"}}));
        BOOST_CHECK(!host.startup(cfg, err));
        auto infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "failed");
        fs::remove_all(root);
    }
    {
        auto root = unique_root("state_started_stopped");
        make_package(root, "fake.test", fake_manifest("fake.test"), true);
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "fake.test"));
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        auto infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "started");
        host.shutdown();
        BOOST_CHECK(host.find_instance("m")->state == State::stopped);
        infos = host.list_instances();
        BOOST_REQUIRE_EQUAL(infos.size(), 1u);
        BOOST_CHECK_EQUAL(infos[0].state, "stopped");
        fs::remove_all(root);
    }
}

BOOST_AUTO_TEST_CASE(startup_stage_failures) {
    if (!fake_ready()) return;
    {
        auto root = unique_root("startup_catalog_fail");
        write_file(root / "a" / "manifest.yaml", fake_manifest("dup.test"));
        write_file(root / "b" / "manifest.yaml", fake_manifest("dup.test"));
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find("duplicate package id") != std::string::npos);
        fs::remove_all(root);
    }
    {
        auto root = unique_root("startup_plan_fail");
        write_file(root / "plain" / "manifest.yaml",
                   fake_manifest("plain.test"));
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("", "plain.test"));
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find("instance id is required") != std::string::npos);
        fs::remove_all(root);
    }
    {
        auto root = unique_root("startup_load_fail");
        write_file(root / "nolib" / "manifest.yaml",
                   fake_manifest("nolib.test"));
        PluginHost host;
        std::string err;
        PluginConfig cfg;
        cfg.directory = root.string();
        cfg.instances.push_back(decl("m", "nolib.test"));
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find("plugin.entry.missing") != std::string::npos);
        fs::remove_all(root);
    }
}

BOOST_AUTO_TEST_CASE(shutdown_with_stale_start_order_is_safe) {
    auto root = unique_root("stale_order");
    write_file(root / "plain" / "manifest.yaml", fake_manifest("plain.test"));
    PluginHost host;
    std::string err;
    host.scan(root.string());
    BOOST_REQUIRE(host.catalog(err));
    PluginConfig cfgA;
    cfgA.directory = root.string();
    cfgA.instances.push_back(decl("a1", "plain.test"));
    cfgA.instances.push_back(decl("a2", "plain.test"));
    BOOST_REQUIRE(host.plan_and_resolve(cfgA, err));
    BOOST_REQUIRE_EQUAL(host.list_instances().size(), 2u);

    PluginConfig cfgB;
    cfgB.directory = root.string();
    cfgB.instances.push_back(decl("", "plain.test"));
    BOOST_CHECK(!host.plan_and_resolve(cfgB, err));
    BOOST_TEST(err.find("instance id is required") != std::string::npos);
    host.shutdown();
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Round-3: shutdown budget exhaustion. Two instances of a plugin whose
// shutdown callback sleeps: with a tiny budget, the first callback burns the
// deadline and the second instance is skipped without invoking its callback.
// ---------------------------------------------------------------------------
const char* kSlowShutdownPluginSource = R"SLOW(#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace {
const void* slow_get_iface(shield_plugin_instance_v1*, const char*,
                           shield_error_v1*) {
    static const int sentinel = 7;
    return &sentinel;
}
int slow_start(shield_plugin_instance_v1*, shield_error_v1*) { return 0; }
void slow_shutdown(shield_plugin_instance_v1* self) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    delete self;
}
int slow_create(const struct shield_plugin_create_args_v1*,
                struct shield_plugin_instance_v1** out,
                struct shield_error_v1*) {
    *out = new shield_plugin_instance_v1{};
    (*out)->struct_size = sizeof(shield_plugin_instance_v1);
    (*out)->get_interface = slow_get_iface;
    (*out)->start = slow_start;
    (*out)->shutdown = slow_shutdown;
    return 0;
}
}  // namespace

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1() {
    static shield_plugin_abi_v1 abi{};
    abi.abi_version = SHIELD_PLUGIN_ABI_VERSION;
    abi.struct_size = sizeof(shield_plugin_abi_v1);
    abi.package_id = "slow.pkg";
    abi.create = slow_create;
    return &abi;
}
)SLOW";

struct SlowShutdownPlugin {
    fs::path so;
    bool ok = false;
    SlowShutdownPlugin() {
        auto dir = fs::temp_directory_path() / "shield_cov_plugin_host" /
                   "slow_plugin";
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto src = dir / "slow_plugin.cpp";
        write_file(src, kSlowShutdownPluginSource);
        so = dir / "libslow_plugin.so";
        std::vector<std::string> compilers;
        if (const char* cxx = std::getenv("CXX")) compilers.push_back(cxx);
        compilers.push_back("/usr/bin/x86_64-linux-gnu-g++-15");
        compilers.push_back("/usr/bin/g++");
        compilers.push_back("/usr/bin/c++");
        const auto inc = source_include_dir().string();
        for (const auto& c : compilers) {
            std::ostringstream cmd;
            cmd << c << " -std=c++17 -shared -fPIC -I\"" << inc << "\" -o \""
                << so.string() << "\" \"" << src.string() << "\" 2>/dev/null";
            if (std::system(cmd.str().c_str()) == 0 && fs::exists(so)) {
                ok = true;
                break;
            }
        }
    }
};

SlowShutdownPlugin& slow_shutdown_plugin() {
    static SlowShutdownPlugin p;
    return p;
}

BOOST_AUTO_TEST_CASE(shutdown_budget_exhaustion_skips_remaining_callbacks) {
    if (!slow_shutdown_plugin().ok) {
        BOOST_TEST_MESSAGE("slow plugin compile unavailable; skipping");
        return;
    }

    auto root = unique_root("slow_shutdown");
    auto manifest =
        fake_manifest("slow.pkg", "shield_plugin_get_v1", "fake.test.iface", "",
                      "bindings_provided: []\n");
    // fake_manifest's linux lib is bin/libfake.so; place the slow .so there.
    auto pkg = root / "slow.pkg";
    fs::create_directories(pkg / "bin");
    write_file(pkg / "manifest.yaml", manifest);
    fs::copy_file(slow_shutdown_plugin().so, pkg / "bin" / "libfake.so");

    PluginConfig pc;
    pc.directory = root.string();
    InstanceDecl a;
    a.id = "slow.a";
    a.package = "slow.pkg";
    InstanceDecl b;
    b.id = "slow.b";
    b.package = "slow.pkg";
    pc.instances.push_back(a);
    pc.instances.push_back(b);

    PluginHost host;
    std::string error;
    if (!host.startup(pc, error)) {
        std::ostringstream diag;
        diag << "startup failed: " << error;
        for (const auto& inst : host.instances()) {
            diag << " [" << inst.id << " state=" << static_cast<int>(inst.state)
                 << " err=" << inst.last_error << "]";
        }
        BOOST_REQUIRE_MESSAGE(false, diag.str());
    }

    // Tiny budget: the first shutdown callback (400ms) exhausts it, so the
    // second instance is skipped without calling into the plugin.
    host.shutdown(150);
    for (const auto& inst : host.instances()) {
        BOOST_CHECK(inst.state != State::started);
    }
}

// ---------------------------------------------------------------------------
// Round-4: catalog validation branches, required-instance failure paths,
// duplicate bindings, dependency cycles, and schema-validated config.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(catalog_rejects_empty_provides) {
    auto root = unique_root("no_provides");
    make_package(root, "nopkg", fake_manifest("nopkg", "fake_entry_ok", ""),
                 true);
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(PluginConfig{root.string()}, err));
    BOOST_CHECK(err.find("at least one provided interface") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(catalog_rejects_empty_interface_name) {
    auto root = unique_root("empty_iface");
    auto m = fake_manifest("emptyiface", "fake_entry_ok", "placeholder");
    // Overwrite the provides block with an empty interface name.
    m.replace(m.find("provides:"), std::string::npos,
              "provides:\n  - interface: ''\n");
    make_package(root, "emptyiface", m, true);
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(PluginConfig{root.string()}, err));
    BOOST_CHECK(err.find("empty provided interface") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(catalog_rejects_duplicate_interface) {
    auto root = unique_root("dup_iface");
    auto m = fake_manifest("dupiface", "fake_entry_ok", "placeholder");
    m.replace(
        m.find("provides:"), std::string::npos,
        "provides:\n  - interface: dup.iface\n  - interface: dup.iface\n");
    make_package(root, "dupiface", m, true);
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(PluginConfig{root.string()}, err));
    BOOST_CHECK(err.find("duplicate interface") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(required_package_missing_fails_startup) {
    if (!fake_ready()) return;
    auto root = unique_root("missing_pkg");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("req", "no.such.package"));
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("plugin.package.not_found") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(required_start_failure_fails_startup) {
    if (!fake_ready()) return;
    auto root = unique_root("req_start_fail");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("reqstart", "fake.test", true, {},
                                 nlohmann::json{{"mode", "start_fail"}}));
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("plugin.init.failed") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(duplicate_binding_rejected) {
    if (!fake_ready()) return;
    auto root = unique_root("dup_binding");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("b1", "fake.test"));
    BindingDecl b1;
    b1.logical = "same.binding";
    b1.instance_id = "b1";
    BindingDecl b2;
    b2.logical = "same.binding";
    b2.instance_id = "b1";
    cfg.bindings.push_back(b1);
    cfg.bindings.push_back(b2);
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("duplicate binding") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(dependency_cycle_rejected) {
    if (!fake_ready()) return;
    auto root = unique_root("dep_cycle");
    // The package requires a "peer" interface so both instances resolve a
    // dependency edge and form a cycle.
    make_package(root, "fake.test",
                 fake_manifest("fake.test", "fake_entry_ok", "fake.test.iface",
                               "  - name: peer\n    interface: "
                               "fake.test.iface\n"),
                 true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("x", "fake.test", true, {{"peer", "y"}}));
    cfg.instances.push_back(decl("y", "fake.test", true, {{"peer", "x"}}));
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("circular dependency") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(config_schema_required_field_missing_fails) {
    if (!fake_ready()) return;
    auto root = unique_root("cfg_required");
    const std::string manifest =
        "schema_version: 1\n"
        "id: cfgreq\n"
        "name: cfgreq\n"
        "version: 1.0.0\n"
        "kind: coverage\n"
        "entry: fake_entry_ok\n"
        "library:\n"
        "  linux: bin/libfake.so\n"
        "  macos: bin/libfake.so\n"
        "provides:\n"
        "  - interface: fake.test.iface\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n"
        "  required:\n"
        "    - port\n";
    make_package(root, "cfgreq", manifest, true);
    PluginConfig cfg;
    cfg.directory = root.string();
    // No "port" in the instance config: validation fails and the required
    // instance aborts startup.
    cfg.instances.push_back(decl("needs_port", "cfgreq"));
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("required field missing") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Round-5: binding targets a missing instance, non-required config-invalid
// instance continues startup, and a failing non-required register_lua.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(binding_to_missing_instance_rejected) {
    if (!fake_ready()) return;
    auto root = unique_root("bind_missing");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("b1", "fake.test"));
    BindingDecl b;
    b.logical = "some.binding";
    b.instance_id = "ghost_instance";
    cfg.bindings.push_back(b);
    PluginHost host;
    std::string err;
    BOOST_CHECK(!host.startup(cfg, err));
    BOOST_CHECK(err.find("targets missing instance") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(non_required_config_invalid_instance_continues) {
    if (!fake_ready()) return;
    auto root = unique_root("cfg_optional");
    const std::string manifest =
        "schema_version: 1\n"
        "id: cfgopt\n"
        "name: cfgopt\n"
        "version: 1.0.0\n"
        "kind: coverage\n"
        "entry: fake_entry_ok\n"
        "library:\n"
        "  linux: bin/libfake.so\n"
        "  macos: bin/libfake.so\n"
        "provides:\n"
        "  - interface: fake.test.iface\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n"
        "  required:\n"
        "    - port\n";
    make_package(root, "cfgopt", manifest, true);
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("healthy", "fake.test"));
    // Non-required instance without the required "port": marked unavailable
    // but startup continues and the healthy instance starts.
    cfg.instances.push_back(decl("sick", "cfgopt", false));
    PluginHost host;
    std::string err;
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    BOOST_CHECK(host.find_instance("healthy")->state == State::started);
    BOOST_CHECK(host.find_instance("sick")->state == State::unavailable);
}

BOOST_AUTO_TEST_CASE(non_required_register_lua_failure_logs_warning) {
    if (!fake_ready()) return;
    auto root = unique_root("register_fail");
    make_package(root, "fake.test", fake_manifest("fake.test"), true);
    PluginConfig cfg;
    cfg.directory = root.string();
    cfg.instances.push_back(decl("failer", "fake.test", false, {},
                                 nlohmann::json{{"mode", "register_fail"}}));
    PluginHost host;
    std::string err;
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);

    auto L = make_lua();
    BOOST_REQUIRE(L);
    BOOST_CHECK(host.register_lua_all(L.get(), err));
}
