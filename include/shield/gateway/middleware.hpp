// [CORE]
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "shield/gateway/gateway_message.hpp"

namespace shield::gateway {

// A middleware receives the request, response, and a `next` callback.
// Call `next()` to continue the chain, or skip it to short-circuit.
using Middleware = std::function<void(GatewayRequest& req,
                                     GatewayResponse& resp,
                                     std::function<void()>& next)>;

// Ordered chain of middleware.  execute() walks the list, calling each
// middleware in order and then invoking the final handler.
class MiddlewareChain {
public:
    // Append a middleware to the end of the chain.
    void use(Middleware mw);

    // Run every middleware in order, then call `final_handler`.
    // If any middleware skips `next`, the chain stops early.
    void execute(GatewayRequest& req,
                 GatewayResponse& resp,
                 std::function<void(GatewayRequest&, GatewayResponse&)> final_handler);

private:
    std::vector<Middleware> middlewares_;
};

// --- Built-in middleware factories ---

// Log every request (method + path + status code).
Middleware logging_middleware();

// Add CORS headers to every HTTP response.
Middleware cors_middleware(const std::string& allow_origin = "*",
                           const std::string& allow_methods = "GET,POST,PUT,DELETE,OPTIONS",
                           const std::string& allow_headers = "Content-Type,Authorization");

// Reject requests that fail `validator`.  Returns 401 on failure.
Middleware auth_middleware(
    std::function<bool(const GatewayRequest&)> validator);

}  // namespace shield::gateway
