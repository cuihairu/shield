#include "shield/service/service_api.hpp"

#include <memory>
#include <thread>

#include "caf/scoped_actor.hpp"
#include "shield/log/logger.hpp"
#include "shield/service/service_context.hpp"

namespace shield::service {

namespace {
caf::actor g_timer_actor;
}  // namespace

void send(const ServiceHandle& target, const std::string& type,
          const std::string& payload) {
    if (!target.valid()) {
        SHIELD_LOG_ERROR << "service::send() called with invalid handle";
        return;
    }
    caf::anon_send(target.caf_handle(), type, payload);
}

void send(const std::string& target_name, const std::string& type,
          const std::string& payload) {
    if (!ServiceContext::has_current()) {
        SHIELD_LOG_ERROR << "service::send() no ServiceContext available";
        return;
    }
    auto handle = query(target_name);
    if (handle.valid()) {
        send(handle, type, payload);
    } else {
        SHIELD_LOG_ERROR << "service::send() target not found: " << target_name;
    }
}

std::future<std::string> call(const ServiceHandle& target,
                              const std::string& type,
                              const std::string& payload,
                              std::chrono::milliseconds timeout_ms) {

    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    if (!target.valid()) {
        promise->set_exception(
            std::make_exception_ptr(std::runtime_error("Invalid handle")));
        return future;
    }

    auto& ctx = ServiceContext::current();

    if (ctx.self()) {
        ctx.self()
            ->request(target.caf_handle(), timeout_ms, type, payload)
            .then(
                [promise](const std::string& response) {
                    promise->set_value(response);
                },
                [promise](caf::error& err) {
                    promise->set_exception(std::make_exception_ptr(
                        std::runtime_error("Call failed: " +
                                           caf::to_string(err))));
                });

    } else {
        std::thread(
            [&sys = ctx.caf_system(), h = target.caf_handle(), type, payload,
             timeout_ms, promise]() {

            try {
                caf::scoped_actor scoped(sys);
                scoped
                    ->request(h, timeout_ms, type, payload)
                    .receive(
                        [promise](const std::string& response) {
                            promise->set_value(response);
                        },
                        [promise](caf::error& err) {
                            promise->set_exception(
                                std::make_exception_ptr(std::runtime_error(
                                    "Call failed: " + caf::to_string(err))));
                        });
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        }).detach();
    }

    return future;
}

std::future<std::string> call(const std::string& target_name,
                              const std::string& type,
                              const std::string& payload,
                              std::chrono::milliseconds timeout_ms) {

    auto handle = query(target_name);
    if (!handle.valid()) {
        auto promise = std::make_shared<std::promise<std::string>>();
        promise->set_exception(std::make_exception_ptr(
            std::runtime_error("Service not found: " + target_name)));
        return promise->get_future();
    }
    return call(handle, type, payload, timeout_ms);
}

caf::disposable timeout(std::chrono::milliseconds ms,
                        std::function<void()> callback) {
    if (!ServiceContext::has_current()) {
        SHIELD_LOG_ERROR << "service::timeout() no ServiceContext available";
        return {};
    }
    auto& ctx = ServiceContext::current();
    auto& sys = ctx.caf_system();

    if (ctx.self()) {
        return ctx.self()->schedule(ctx.self()->clock().now() + ms,
                                    caf::action::from_lambda(std::move(callback)));
    }

    if (g_timer_actor) {
        caf::anon_send(g_timer_actor, ms, std::move(callback));
    }
    return {};
}

void cancel_timeout(caf::disposable& handle) {
    if (handle) {
        handle.dispose();
        handle.release();
    }
}

void name(const ServiceHandle& handle, const std::string& service_name) {
    if (!handle.valid()) return;
    auto& ctx = ServiceContext::current();
    ctx.distributed_system().register_actor(handle.caf_handle(),
                                            actor::ActorType::CUSTOM,
                                            service_name);
}

ServiceHandle query(const std::string& service_name) {
    auto& ctx = ServiceContext::current();
    auto actor = ctx.distributed_system().find_actor(service_name);
    if (actor) {
        return ServiceHandle(actor, service_name, true);
    }
    return ServiceHandle();
}

ServiceHandle uniqueservice(const std::string& service_name,
                             actor::ActorType type) {
    auto existing = query(service_name);
    if (existing.valid()) return existing;

    auto& ctx = ServiceContext::current();
    auto actor = ctx.caf_system().spawn([](caf::event_based_actor* self) {
        return caf::behavior{
            [](const std::string& type,
               const std::string& data) -> std::string {
                return R"({"success": true})";
            }};
    });
    ctx.distributed_system().register_actor(actor, type, service_name);
    return ServiceHandle(actor, service_name, true);
}

ServiceHandle fork(
    std::function<void(caf::event_based_actor*)> func,
    const std::string& fork_name) {
    auto& ctx = ServiceContext::current();
    auto actor = ctx.caf_system().spawn(
        [f = std::move(f)](caf::event_based_actor* self) {
            f(self);
            return caf::behavior{
                [](const std::string&, const std::string&) {
                    return std::string();
                }};
        });
    ServiceHandle handle(actor, fork_name, true);
    if (!fork_name.empty()) {
        ctx.distributed_system().register_actor(actor, actor::ActorType::CUSTOM,
                                                fork_name);
    }
    return handle;
}

std::vector<std::string> list_services() {
    auto& ctx = ServiceContext::current();
    auto local_actors = ctx.distributed_system()
                            .find_actors_by_type(actor::ActorType::CUSTOM,
                                                 true, false);
    auto gateway_actors = ctx.distributed_system()
                              .find_actors_by_type(actor::ActorType::GATEWAY,
                                                   true, false);
    auto logic_actors = ctx.distributed_system()
                            .find_actors_by_type(actor::ActorType::LOGIC,
                                                 true, false);

    std::vector<std::string> result;
    auto add_names = [&](auto& actors) {
        for (auto& ra : actors) {
            if (!ra.metadata.name.empty())
                result.push_back(ra.metadata.name);
        }
    };
    add_names(local_actors);
    add_names(gateway_actors);
    add_names(logic_actors);
    return result;
}

std::string service_info(const std::string& service_name) {
    auto handle = query(service_name);
    if (!handle.valid()) return "Service not found: " + service_name;
    return handle.to_string();
}

std::string self_node_id() {
    if (!ServiceContext::has_current()) return "unknown";
    return ServiceContext::current().node_id();
}

}  // namespace shield::service
