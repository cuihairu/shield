#pragma once

#include <chrono>
#include <memory>

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/service.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/gateway/gateway_request_dispatcher.hpp"
#include "shield/http/beast_http_server.hpp"
#include "shield/net/master_reactor.hpp"
#include "shield/net/session.hpp"
#include "shield/protocol/binary_protocol.hpp"
#include "shield/protocol/http_handler.hpp"
#include "shield/protocol/protocol_handler.hpp"
#include "shield/protocol/websocket_handler.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/serialization/universal_serializer.hpp"

namespace shield::gateway {

class GatewayService : public core::ReloadableService {
public:
    explicit GatewayService(const std::string &name,
                            actor::DistributedActorSystem &actor_system,
                            script::LuaVMPool &lua_vm_pool,
                            std::shared_ptr<GatewayConfig> config);
    ~GatewayService();

protected:
    void on_init(core::ApplicationContext &ctx) override;
    void on_start() override;
    void on_stop() override;

    // ReloadableService interface
    void on_config_reloaded() override;

public:
    std::string name() const override { return m_name; }

private:
    void setup_session(std::shared_ptr<net::Session> session);
    void send_binary_json_response(uint64_t session_id,
                                   const std::string& response);
    void setup_protocol_handlers();
    std::shared_ptr<net::Session> get_session(uint64_t session_id);
    protocol::ProtocolType detect_protocol(uint64_t connection_id,
                                           const char *data, size_t length);

    actor::DistributedActorSystem &m_actor_system;
    script::LuaVMPool &m_lua_vm_pool;
    std::shared_ptr<GatewayConfig> m_config;
    std::string m_name;

    std::unique_ptr<net::MasterReactor> m_master_reactor;
    std::unique_ptr<net::MasterReactor> m_http_reactor;  // HTTP server
    std::unique_ptr<net::MasterReactor> m_ws_reactor;    // WebSocket server
    std::unique_ptr<http::BeastHttpServer> m_beast_http_server;
    std::unique_ptr<GatewayRequestDispatcher> m_request_dispatcher;

    std::unordered_map<uint64_t, std::vector<char>> m_session_recv_buffers;
    std::unordered_map<uint64_t, std::weak_ptr<net::Session>>
        m_sessions;  // Map session ID to Session
    std::unordered_map<uint64_t, protocol::ProtocolType>
        m_session_protocols;  // Track protocol per session

    // Protocol handlers
    std::unique_ptr<protocol::HttpProtocolHandler> m_http_handler;
    std::unique_ptr<protocol::WebSocketProtocolHandler> m_websocket_handler;

    // Request timeout configuration
    std::chrono::milliseconds m_request_timeout{
        5000};  // 5 seconds default timeout
};

}  // namespace shield::gateway
