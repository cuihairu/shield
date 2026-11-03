// BUG-5 regression: leaderboard.redis create_board must copy the caller's
// field-name pointers instead of storing them borrowed. The C vtable's
// create_board takes a shield_leaderboard_config whose field_defs[].name
// points at caller-owned memory (often a stack buffer that is released or
// overwritten after the call returns); a borrowed name would dangle on the
// next set_entry / get_entry. This test overwrites the stack buffer after
// create_board and checks get_entry still reports the original field name.
//
// Runs against a real redis-server (skipped when unavailable) through the
// plugin ABI + C vtable.
#define BOOST_TEST_MODULE LeaderboardNames
#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/plugin_library.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using shield::plugin::PluginLibrary;

#ifndef _WIN32
struct redis_server {
    int port = 0;
    ::pid_t pid = -1;
    ~redis_server() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

int probe_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_accepts(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

std::unique_ptr<redis_server> spawn_redis_server() {
    const int port = probe_free_port();
    if (port <= 0) return nullptr;
    const ::pid_t pid = ::fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        const auto port_str = std::to_string(port);
        ::execlp("redis-server", "redis-server", "--port", port_str.c_str(),
                 "--save", "", "--appendonly", "no", nullptr);
        ::_exit(127);
    }
    for (int i = 0; i < 100; ++i) {
        if (tcp_accepts(port)) {
            auto srv = std::make_unique<redis_server>();
            srv->port = port;
            srv->pid = pid;
            return srv;
        }
        ::usleep(50000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return nullptr;
}
#endif

}  // namespace

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(create_board_copies_field_names) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }

    std::string err;
    PluginLibrary lib = PluginLibrary::load(SHIELD_LEADERBOARD_LIBRARY, err);
    BOOST_REQUIRE_MESSAGE(lib.is_loaded(), err);

    using get_v1_fn = const shield_plugin_abi_v1* (*)();
    void* sym = lib.resolve("shield_plugin_get_v1");
    BOOST_REQUIRE_MESSAGE(sym != nullptr, "entry symbol missing");
    const shield_plugin_abi_v1* abi = reinterpret_cast<get_v1_fn>(sym)();
    BOOST_REQUIRE(abi != nullptr);

    shield_plugin_create_args_v1 cargs{};
    cargs.instance_id = "lb_names_test";
    cargs.config_json = "{}";
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&cargs, &inst, nullptr), 0);
    BOOST_REQUIRE(inst != nullptr);

    const auto* lb = static_cast<const shield_leaderboard_v1*>(
        inst->get_interface(inst, SHIELD_LEADERBOARD_INTERFACE, nullptr));
    BOOST_REQUIRE(lb != nullptr);

    shield_leaderboard_connect_args carg{};
    carg.host = "127.0.0.1";
    carg.port = srv->port;
    carg.connect_timeout_ms = 5000;
    carg.command_timeout_ms = 5000;
    char conn_err[256] = {};
    auto* conn = lb->connect(&carg, conn_err, sizeof(conn_err));
    BOOST_REQUIRE_MESSAGE(conn != nullptr, conn_err);

    // Stack-backed field name: after create_board returns we overwrite it to
    // prove the board keeps its own copy.
    char field_name[64] = {};
    std::strcpy(field_name, "score");
    shield_leaderboard_field_def defs[1];
    defs[0].name = field_name;
    defs[0].dir = SHIELD_SORT_DESC;
    shield_leaderboard_config config{};
    config.name = "board_owner";
    config.field_defs = defs;
    config.field_count = 1;
    config.max_entries = 1000;
    char b_err[256] = {};
    BOOST_REQUIRE_EQUAL(lb->create_board(conn, &config, b_err, sizeof(b_err)),
                        0);

    // Overwrite the caller's buffer: the board must not observe this.
    std::memset(field_name, 'X', sizeof(field_name));

    // set_entry / get_entry exercise the cached BoardConfig (which holds the
    // copied name).
    const char* names[] = {"score"};
    double vals[] = {42.0};
    BOOST_REQUIRE_EQUAL(
        lb->set_entry(conn, "board_owner", "p1", names, vals, 1), 0);

    shield_leaderboard_entry entry{};
    BOOST_REQUIRE_EQUAL(lb->get_entry(conn, "board_owner", "p1", &entry), 0);
    BOOST_REQUIRE(entry.field_count >= 1);
    BOOST_REQUIRE(entry.field_names != nullptr);
    BOOST_REQUIRE(entry.field_names[0] != nullptr);
    BOOST_CHECK_EQUAL(std::string(entry.field_names[0]), "score");

    lb->free_entry(&entry);
    lb->disconnect(conn);
    inst->shutdown(inst);
}
#endif
