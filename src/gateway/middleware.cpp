#include "shield/gateway/middleware.hpp"

#include "shield/log/logger.hpp"

namespace shield::gateway {

void MiddlewareChain::use(Middleware mw) {
    middlewares_.push_back(std::move(mw));
}

void MiddlewareChain::execute(
    GatewayRequest& req,
    GatewayResponse& resp,
    std::function<void(GatewayRequest&, GatewayResponse&)> final_handler) {
    if (middlewares_.empty()) {
        final_handler(req, resp);
        return;
    }

    // Build a linked chain of calls: mw[0] → mw[1] → … → final_handler.
    size_t index = 0;
    std::function<void()> next;

    // We rebuild `next` recursively.  Each level captures its own index.
    // Using a pointer to the shared `next` so each level can advance.
    auto build = [&](auto& self, size_t i) -> std::function<void()> {
        if (i < middlewares_.size()) {
            return [this, &req, &resp, &next, i]() {
                // `next` is updated before calling the middleware so that
                // the middleware sees the *next* step when it calls next().
                next = [this, &req, &resp, &final_handler, i]() {
                    // Advance past middleware i and run the rest.
                    // We simply call the next middleware or the final handler.
                    if (i + 1 < middlewares_.size()) {
                        middlewares_[i + 1](req, resp, next);
                    } else {
                        final_handler(req, resp);
                    }
                };
                middlewares_[i](req, resp, next);
            };
        }
        return [&]() { final_handler(req, resp); };
    };

    // Simplified: iterate through middlewares with an explicit index.
    // Re-implement with a cleaner iterative approach.
    next = [&]() { final_handler(req, resp); };

    // Build chain from back to front.
    for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
        auto saved_next = next;
        next = [this, &req, &resp, saved_next, i]() {
            middlewares_[i](req, resp, saved_next);
        };
    }

    next();
}

// --- Built-in middleware implementations ---

Middleware logging_middleware() {
    return [](GatewayRequest& req, GatewayResponse& resp,
              std::function<void()>& next) {
        SHIELD_LOG_INFO << ">> " << req.method << " " << req.path
                        << " proto=" << static_cast<int>(req.protocol)
                        << " session=" << req.session_id;
        next();
        SHIELD_LOG_INFO << "<< " << req.method << " " << req.path
                        << " status=" << resp.status_code
                        << " success=" << resp.success;
    };
}

Middleware cors_middleware(const std::string& allow_origin,
                           const std::string& allow_methods,
                           const std::string& allow_headers) {
    return [allow_origin, allow_methods, allow_headers](
               GatewayRequest& req, GatewayResponse& resp,
               std::function<void()>& next) {
        resp.headers["Access-Control-Allow-Origin"] = allow_origin;
        resp.headers["Access-Control-Allow-Methods"] = allow_methods;
        resp.headers["Access-Control-Allow-Headers"] = allow_headers;

        // Short-circuit preflight requests
        if (req.method == "OPTIONS") {
            resp.status_code = 204;
            resp.success = true;
            return;  // skip next
        }
        next();
    };
}

Middleware auth_middleware(
    std::function<bool(const GatewayRequest&)> validator) {
    return [validator](GatewayRequest& req, GatewayResponse& resp,
                       std::function<void()>& next) {
        if (!validator(req)) {
            resp.success = false;
            resp.status_code = 401;
            resp.body = R"({"success":false,"error":"unauthorized"})";
            return;  // short-circuit
        }
        next();
    };
}

}  // namespace shield::gateway
