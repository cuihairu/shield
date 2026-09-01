// [SHIELD_LUA] Bridge inbound HTTP requests to Lua service handlers
#pragma once

#include <mutex>
#include <string>

#include "shield/net/http_server.hpp"

namespace shield::lua {

class LuaRuntime;
class LuaServiceManager;

/// @brief Connects shield.httpd.* route registrations to a running HttpServer.
///
/// Requests are dispatched onto the registering service's actor thread via
/// LuaServiceManager::enqueue_forked_task, so Lua handlers run on the same
/// serialized dispatch path as normal service messages (no VM races). The
/// HTTP io thread blocks on a bounded future (see kDispatchTimeoutMs).
class LuaHttpBridge {
public:
    LuaHttpBridge(LuaRuntime& runtime, LuaServiceManager& manager);
    ~LuaHttpBridge();

    // Non-copyable
    LuaHttpBridge(const LuaHttpBridge&) = delete;
    LuaHttpBridge& operator=(const LuaHttpBridge&) = delete;

    /// Mirror every route currently registered in the runtime into `server`
    /// and subscribe to later registrations. Call before HttpServer::start().
    void attach(shield::net::HttpServer& server);

    /// Stop reacting to new registrations (call before the server dies).
    void detach();

    /// Request entry point registered as the HttpServer handler.
    shield::net::HttpResponse handle(const shield::net::HttpRequest& req);

private:
    void register_on_server(const std::string& method, const std::string& path);

    LuaRuntime& runtime_;
    LuaServiceManager& manager_;
    std::mutex server_mutex_;
    shield::net::HttpServer* server_ = nullptr;
};

}  // namespace shield::lua
