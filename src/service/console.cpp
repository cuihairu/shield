#include "shield/service/console.hpp"

#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define SOCKET_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET ::close
#define SOCKET_INVALID (-1)
#endif

#include "shield/log/logger.hpp"
#include "shield/service/service_api.hpp"
#include "shield/service/service_context.hpp"

namespace shield::service {

DebugConsole::DebugConsole(const std::string& svc_name, uint16_t port)
    : name_(svc_name), port_(port) {}

DebugConsole::~DebugConsole() { on_stop(); }

void DebugConsole::on_init(core::ApplicationContext& ctx) {
    dist_system_ = ctx.get_service<actor::DistributedActorSystem>().get();
}

void DebugConsole::on_start() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    listen_fd_ =
        static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));

    if (listen_fd_ == SOCKET_INVALID) {
        SHIELD_LOG_ERROR << "DebugConsole: failed to create socket";
        return;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        SHIELD_LOG_ERROR << "DebugConsole: bind failed on port " << port_;
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    ::listen(listen_fd_, 4);
    running_ = true;
    accept_thread_ = std::thread(&DebugConsole::accept_loop, this);

    SHIELD_LOG_INFO << "DebugConsole listening on port " << port_;
}

void DebugConsole::on_stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    SHIELD_LOG_INFO << "DebugConsole stopped";
}

void DebugConsole::accept_loop() {
    while (running_) {
        struct sockaddr_in client_addr {};
        socklen_t len = sizeof(client_addr);
        int client_fd =
            static_cast<int>(::accept(
                listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
                &len));

        if (client_fd == SOCKET_INVALID) continue;
        handle_client(client_fd);
    }
}

void DebugConsole::handle_client(int client_fd) {
    auto send_str = [client_fd](const std::string& s) {
        ::send(client_fd, s.c_str(), s.size(), 0);
    };

    send_str("Shield Debug Console. Type 'help' for commands.\n> ");

    char buf[1024];
    while (running_) {
        int n = static_cast<int>(::recv(client_fd, buf, sizeof(buf) - 1, 0));
        if (n <= 0) break;
        buf[n] = '\0';

        std::string cmd(buf);
        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n'))
            cmd.pop_back();

        if (cmd == "quit" || cmd == "exit") break;

        std::string response = process_command(cmd);
        send_str(response + "\n> ");
    }

    CLOSE_SOCKET(client_fd);
}

std::string DebugConsole::process_command(const std::string& command) {
    std::istringstream iss(command);
    std::string op;
    iss >> op;

    if (op == "help") return cmd_help();
    if (op == "list") return cmd_list();
    if (op == "nodes") return cmd_nodes();

    std::string arg1, arg2;
    if (op == "info") {
        iss >> arg1;
        return cmd_info(arg1);
    }
    if (op == "send") {
        iss >> arg1;
        std::getline(iss, arg2);
        while (!arg2.empty() && arg2.front() == ' ') arg2.erase(arg2.begin());
        return cmd_send(arg1, arg2);
    }
    if (op == "call") {
        iss >> arg1;
        std::getline(iss, arg2);
        while (!arg2.empty() && arg2.front() == ' ') arg2.erase(arg2.begin());
        return cmd_call(arg1, arg2);
    }

    return "Unknown command: " + op + ". Type 'help'.";
}

std::string DebugConsole::cmd_help() {
    return "Commands:\n"
           "  list              - List all registered services\n"
           "  info <name>       - Show service details\n"
           "  send <name> <msg> - Send message (fire-and-forget)\n"
           "  call <name> <msg> - Call service (sync, 5s timeout)\n"
           "  nodes             - Show cluster topology\n"
           "  help              - Show this help\n"
           "  quit              - Disconnect";
}

std::string DebugConsole::cmd_list() {
    if (!ServiceContext::has_current()) return "ServiceContext not available";
    auto services = list_services();
    if (services.empty()) return "(no services registered)";
    std::string result;
    for (auto& s : services) result += "  " + s + "\n";
    return result;
}

std::string DebugConsole::cmd_info(const std::string& service_name) {
    if (!ServiceContext::has_current()) return "ServiceContext not available";
    return service_info(service_name);
}

std::string DebugConsole::cmd_send(const std::string& target,
                                   const std::string& msg) {
    if (!ServiceContext::has_current()) return "ServiceContext not available";
    send(target, "debug", msg);
    return "Sent to " + target;
}

std::string DebugConsole::cmd_call(const std::string& target,
                                   const std::string& msg) {

    if (!ServiceContext::has_current()) return "ServiceContext not available";
    auto future = call(target, "debug", msg);
    if (future.wait_for(std::chrono::milliseconds(5500)) ==
        std::future_status::ready) {
        return "Response: " + future.get();
    }
    return "Timeout calling " + target;
}

std::string DebugConsole::cmd_nodes() {
    if (!dist_system_) return "Not initialized";
    auto topo = dist_system_->get_cluster_topology();
    if (topo.empty()) return self_node_id() + " (single node)";
    std::string result;
    for (auto& [node, actors] : topo) {
        result += node + ": ";
        for (auto& a : actors) result += a + " ";
        result += "\n";
    }
    return result;
}

}  // namespace shield::service
