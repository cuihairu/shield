// [CORE]
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "shield/core/service.hpp"
#include "shield/actor/distributed_actor_system.hpp"

namespace shield::service {

class DebugConsole : public core::Service {
public:
    DebugConsole(const std::string& svc_name, uint16_t port = 13000);
    ~DebugConsole();

    void on_init(core::ApplicationContext& ctx) override;
    void on_start() override;
    void on_stop() override;
    std::string name() const override { return name_; }

private:
    void accept_loop();
    void handle_client(int client_fd);
    std::string process_command(const std::string& command);

    std::string cmd_list();
    std::string cmd_info(const std::string& service_name);
    std::string cmd_send(const std::string& target, const std::string& msg);
    std::string cmd_call(const std::string& target, const std::string& msg);
    std::string cmd_nodes();
    std::string cmd_help();

    std::string name_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    actor::DistributedActorSystem* dist_system_ = nullptr;
};

}  // namespace shield::service
