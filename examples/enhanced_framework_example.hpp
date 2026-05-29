#pragma once

#include "shield/annotations/component_registry.hpp"
#include "shield/conditions/conditional_registry.hpp"
#include "shield/core/service.hpp"
#include "shield/di/advanced_container.hpp"
#include "shield/events/event_system.hpp"
#include "shield/extensions/extension_context.hpp"
#include "shield/health/health_check.hpp"

namespace shield::examples {

class ExampleService : public core::Service {
public:
    ExampleService() = default;

    void on_init(core::ApplicationContext& ctx) override {
        SHIELD_LOG_INFO << "ExampleService initializing...";

        auto& event_publisher =
            extensions::ExtensionContext::instance().get_event_publisher();
        event_publisher.on<events::lifecycle::ApplicationStartedEvent>(
            [this](const auto& event) {
                this->on_application_started(event);
            },
            false, 0);
    }

    void on_start() override { SHIELD_LOG_INFO << "ExampleService started"; }

    void on_stop() override { SHIELD_LOG_INFO << "ExampleService stopped"; }

    std::string name() const override { return "ExampleService"; }

    void do_something() {
        SHIELD_LOG_INFO << "ExampleService doing something...";
    }

private:
    void on_application_started(
        const events::lifecycle::ApplicationStartedEvent& event) {
        SHIELD_LOG_INFO << "ExampleService received ApplicationStartedEvent";
    }
};

class ExampleComponent {
public:
    ExampleComponent() { SHIELD_LOG_INFO << "ExampleComponent created"; }

    void process() { SHIELD_LOG_INFO << "ExampleComponent processing..."; }
};

SHIELD_COMPONENT(ExampleComponent)

class ConditionalService : public core::Service {
public:
    void on_init(core::ApplicationContext& ctx) override {
        SHIELD_LOG_INFO << "ConditionalService initialized (condition was met)";
    }

    void on_start() override {
        SHIELD_LOG_INFO << "ConditionalService started";
    }

    void on_stop() override { SHIELD_LOG_INFO << "ConditionalService stopped"; }

    std::string name() const override { return "ConditionalService"; }
};

SHIELD_CONDITIONAL_ON_PROPERTY(ConditionalService,
                               "features.conditional-service.enabled", "true")

class ExampleHealthIndicator : public health::HealthIndicator {
public:
    health::Health check() override {
        bool is_healthy = true;

        if (is_healthy) {
            return health::Health(health::HealthStatus::UP,
                                  "Example service is healthy")
                .add_detail("status", "operational")
                .add_detail("connections", "5")
                .add_detail("last_check", "2024-01-01T12:00:00Z");
        } else {
            return health::Health(health::HealthStatus::DOWN,
                                  "Example service is down")
                .add_detail("error", "Connection failed");
        }
    }

    std::string name() const override { return "example"; }

    std::chrono::milliseconds timeout() const override {
        return std::chrono::milliseconds(3000);
    }
};

SHIELD_HEALTH_INDICATOR(ExampleHealthIndicator)

class ExampleConfiguration {
public:
    ExampleConfiguration() {
        SHIELD_LOG_INFO << "ExampleConfiguration created";
    }

    std::shared_ptr<ExampleComponent> example_component() {
        return std::make_shared<ExampleComponent>();
    }

    std::shared_ptr<ExampleService> example_service() {
        return std::make_shared<ExampleService>();
    }
};

SHIELD_CONFIGURATION(ExampleConfiguration)

class ExampleEventListener {
public:
    ExampleEventListener() {
        auto& event_publisher =
            extensions::ExtensionContext::instance().get_event_publisher();

        event_publisher.on<events::lifecycle::ApplicationStartedEvent>(
            [this](const auto& event) { this->handle_app_started(event); },
            true, 0);

        event_publisher.on<events::config::ConfigRefreshEvent>(
            [this](const auto& event) { this->handle_config_refresh(event); },
            false, 0);
    }

private:
    void handle_app_started(
        const events::lifecycle::ApplicationStartedEvent& event) {
        SHIELD_LOG_INFO << "ExampleEventListener: Application started!";
    }

    void handle_config_refresh(
        const events::config::ConfigRefreshEvent& event) {
        SHIELD_LOG_INFO << "ExampleEventListener: Configuration refreshed!";
    }
};

class AdvancedDIExample {
public:
    static void demonstrate() {
        auto& ext_ctx = extensions::ExtensionContext::instance();
        auto& container = ext_ctx.get_di_container();

        container.register_auto_inject<ExampleService>(
            di::ServiceLifetime::SINGLETON);
        container.register_auto_inject<ExampleComponent>(
            di::ServiceLifetime::TRANSIENT);

        container.register_factory_advanced<ExampleEventListener>(
            [](di::AdvancedContainer& c) {
                return std::make_shared<ExampleEventListener>();
            },
            di::ServiceLifetime::SINGLETON);

        auto service = container.resolve<ExampleService>();
        auto component1 = container.resolve<ExampleComponent>();
        auto component2 = container.resolve<ExampleComponent>();
        auto listener = container.resolve<ExampleEventListener>();

        SHIELD_LOG_INFO << "Service resolved: " << (service ? "Yes" : "No");
        SHIELD_LOG_INFO << "Components are different instances: "
                        << (component1 != component2 ? "Yes" : "No");
        SHIELD_LOG_INFO << "Listener resolved: " << (listener ? "Yes" : "No");

        if (service) {
            service->do_something();
        }

        if (component1) {
            component1->process();
        }
    }
};

class ExampleApplication {
public:
    static void run() {
        SHIELD_LOG_INFO
            << "Starting Example Application with enhanced Shield framework";

        auto& context = core::ApplicationContext::instance();
        auto& ext_ctx = extensions::ExtensionContext::instance();

        ext_ctx.configure_with_annotations(context);
        ext_ctx.configure_with_conditional_beans(context);

        health::HealthCheckRegistry::instance().register_health_indicator(
            std::make_unique<ExampleHealthIndicator>());

        AdvancedDIExample::demonstrate();

        context.init_all();
        context.start_all();

        ext_ctx.publish_application_started_event();

        auto overall_health =
            health::HealthCheckRegistry::instance().get_overall_health();
        SHIELD_LOG_INFO << "Overall health: "
                        << (overall_health.is_healthy() ? "HEALTHY"
                                                        : "UNHEALTHY");

        std::this_thread::sleep_for(std::chrono::seconds(2));

        ext_ctx.publish_application_stopped_event();
        context.stop_all();

        SHIELD_LOG_INFO << "Example Application finished";
    }
};

}  // namespace shield::examples
