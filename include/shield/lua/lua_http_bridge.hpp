// [SHIELD_LUA] Bridge inbound HTTP requests to Lua service handlers
#pragma once

#include <memory>
#include <string>

#include "shield/net/http_server.hpp"

namespace shield::lua {

class LuaRuntime;
class LuaServiceManager;

/// Shared route-slot state backing the bridge callbacks (defined in the .cpp).
struct LuaHttpBridgeState;

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
    // Held by shared_ptr on purpose: the route sink is copied out by
    // LuaRuntime::register_http_route and invoked after that mutex is
    // released, and per-route handlers sit in the HttpServer route table
    // until the server dies (bootstrap resets this bridge before
    // HttpServer::stop()). Both callback kinds can therefore run after the
    // LuaHttpBridge object is destroyed; they keep the state alive and treat
    // a null server slot as "detached" instead of touching a freed bridge.
    std::shared_ptr<LuaHttpBridgeState> state_;
};

}  // namespace shield::lua
