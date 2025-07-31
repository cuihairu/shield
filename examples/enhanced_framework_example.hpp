#pragma once

#include "shield/annotations/component_registry.hpp"
#include "shield/conditions/conditional_registry.hpp"
#include "shield/core/service.hpp"
#include "shield/di/advanced_container.hpp"
#include "shield/events/event_system.hpp"
#include "shield/health/health_check.hpp"

namespace shield::examples {

/**
 * @brief Example service using new framework features
 */
class ExampleService : public core::Service {
public:
    ExampleService() = default;

    void on_init(core::ApplicationContext& ctx) override {
        SHIELD_LOG_INFO << "ExampleService initializing...";

        // Register event listeners
        auto& event_publisher = ctx.get_event_publisher();
        event_publisher
            .register_listener<events::lifecycle::ApplicationStartedEvent>(
                [this](const auto& event) {
                    this->on_application_started(event);
                },
                events::EventPriority::HIGH,
                false,  // synchronous
                "ExampleService::on_application_started");
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

/**
 * @brief Example component using annotations
 */
class ExampleComponent {
public:
    ExampleComponent() { SHIELD_LOG_INFO << "ExampleComponent created"; }

    void process() { SHIELD_LOG_INFO << "ExampleComponent processing..."; }
};

// Register component using annotation
SHIELD_COMPONENT(ExampleComponent)

/**
 * @brief Example conditional service
 */
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

// Register conditionally based on property
SHIELD_CONDITIONAL_ON_PROPERTY(ConditionalService,
                               "features.conditional-service.enabled", "true")

/**
 * @brief Example health indicator
 */
class ExampleHealthIndicator : public health::HealthIndicator {
public:
    health::Health check() override {
        // Simulate some health check logic
        bool is_healthy = true;  // Your actual health check logic here

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
        return std::chrono::milliseconds(3000);  // 3 second timeout
    }
};

// Register health indicator
SHIELD_HEALTH_INDICATOR(ExampleHealthIndicator)

/**
 * @brief Example configuration class
 */
class ExampleConfiguration {
public:
    ExampleConfiguration() {
        SHIELD_LOG_INFO << "ExampleConfiguration created";
    }

    // This would be a @Bean method in Spring Boot
    std::shared_ptr<ExampleComponent> example_component() {
        return std::make_shared<ExampleComponent>();
    }

    // Another bean factory method
    std::shared_ptr<ExampleService> example_service() {
        return std::make_shared<ExampleService>();
    }
};

// Register as configuration class
SHIELD_CONFIGURATION(ExampleConfiguration)

/**
 * @brief Example event listener class
 */
class ExampleEventListener {
public:
    ExampleEventListener() {
        // Register for application lifecycle events
        auto& event_publisher = events::get_global_event_publisher();

        event_publisher
            .register_listener<events::lifecycle::ApplicationStartedEvent>(
                [this](const auto& event) { this->handle_app_started(event); },
                events::EventPriority::NORMAL,
                true,  // async
                "ExampleEventListener::handle_app_started");

        event_publisher.register_listener<events::config::ConfigRefreshEvent>(
            [this](const auto& event) { this->handle_config_refresh(event); },
            events::EventPriority::HIGH,
            false,  // sync
            "ExampleEventListener::handle_config_refresh");
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

/**
 * @brief Example of using advanced DI container
 */
class AdvancedDIExample {
public:
    static void demonstrate(core::ApplicationContext& context) {
        auto& container = context.get_di_container();

        // Register services with different lifetimes
        container.register_auto_inject<ExampleService>(
            di::ServiceLifetime::SINGLETON);
        container.register_auto_inject<ExampleComponent>(
            di::ServiceLifetime::TRANSIENT);

        // Register with custom factory
        container.register_factory_advanced<ExampleEventListener>(
            [](di::AdvancedContainer& c) {
                return std::make_shared<ExampleEventListener>();
            },
            di::ServiceLifetime::SINGLETON);

        // Resolve services
        auto service = container.resolve<ExampleService>();
        auto component1 = container.resolve<ExampleComponent>();
        auto component2 = container.resolve<ExampleComponent>();
        auto listener = container.resolve<ExampleEventListener>();

        SHIELD_LOG_INFO << "Service resolved: " << (service ? "Yes" : "No");
        SHIELD_LOG_INFO << "Components are different instances: "
                        << (component1 != component2 ? "Yes" : "No");
        SHIELD_LOG_INFO << "Listener resolved: " << (listener ? "Yes" : "No");

        // Use the services
        if (service) {
            service->do_something();
        }

        if (component1) {
            component1->process();
        }
    }
};

/**
 * @brief Example complete application setup
 */
class ExampleApplication {
public:
    static void run() {
        SHIELD_LOG_INFO
            << "Starting Example Application with enhanced Shield framework";

        auto& context = core::ApplicationContext::instance();

        // Configure with annotations
        context.configure_with_annotations();

        // Configure with conditional beans
        context.configure_with_conditional_beans();

        // Start event publisher
        context.get_event_publisher().start();

        // Register health indicators
        context.get_health_registry().register_health_indicator(
            std::make_unique<ExampleHealthIndicator>());

        // Demonstrate advanced DI
        AdvancedDIExample::demonstrate(context);

        // Initialize and start all services
        context.init_all();
        context.start_all();

        // Publish application started event
        context.publish_application_started_event();

        // Check overall health
        auto overall_health =
            context.get_health_registry().get_overall_health();
        SHIELD_LOG_INFO << "Overall health: "
                        << (overall_health.is_healthy() ? "HEALTHY"
                                                        : "UNHEALTHY");

        // Simulate running for a while
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Publish config refresh event
        context.get_event_publisher().publish_event(
            events::config::ConfigRefreshEvent());

        // Stop application
        context.publish_application_stopped_event();
        context.stop_all();
        context.get_event_publisher().stop();

        SHIELD_LOG_INFO << "Example Application finished";
    }
};

}  // namespace shield::examples