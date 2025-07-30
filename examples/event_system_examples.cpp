#include "shield/config/event_driven_config_manager.hpp"
#include "shield/core/event_driven_service.hpp"
#include "shield/events/event_publisher.hpp"
#include "shield/events/event_system.hpp"

namespace shield::examples {

// 示例：如何在应用中使用事件系统
class EventDrivenApplicationExample {
public:
    void demonstrate_event_system() {
        // 1. ========== 注册各种事件监听器 ==========

        // 监听配置刷新事件
        events::GlobalEventPublisher::listen<
            events::config::ConfigRefreshEvent>(
            [](const events::config::ConfigRefreshEvent& event) {
                SHIELD_LOG_INFO
                    << "🔄 Configuration refreshed at "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                           event.get_timestamp().time_since_epoch())
                           .count();
            });

        // 监听应用启动事件（异步处理）
        events::GlobalEventPublisher::listen<
            events::lifecycle::ApplicationStartedEvent>(
            [](const events::lifecycle::ApplicationStartedEvent& event) {
                SHIELD_LOG_INFO << "🚀 Application started! Performing "
                                   "post-startup tasks...";
                // 模拟一些启动后的异步任务
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                SHIELD_LOG_INFO << "✅ Post-startup tasks completed";
            },
            true,  // 异步执行
            10     // 低优先级
        );

        // 监听服务就绪事件
        events::GlobalEventPublisher::listen<
            events::lifecycle::ServiceReadyEvent>(
            [](const events::lifecycle::ServiceReadyEvent& event) {
                SHIELD_LOG_INFO << "📡 Service ready: "
                                << event.get_service_name();
            });

        // 2. ========== 发布各种事件 ==========

        SHIELD_LOG_INFO << "🎯 Demonstrating Event System...";

        // 发布应用启动事件
        events::GlobalEventPublisher::emit<
            events::lifecycle::ApplicationStartedEvent>(
            std::string("ApplicationBootstrap"));

        // 发布配置刷新事件
        events::GlobalEventPublisher::emit<events::config::ConfigRefreshEvent>(
            std::string("FileWatcher"));

        // 发布服务就绪事件
        events::GlobalEventPublisher::emit<
            events::lifecycle::ServiceReadyEvent>(
            "GatewayService", std::string("ServiceManager"));

        // 等待异步事件处理完成
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        SHIELD_LOG_INFO << "✨ Event system demonstration completed!";
    }
};

// 示例：Spring Boot风格的配置热重载
class SpringBootStyleConfigReload {
public:
    void setup_config_hot_reload() {
        // 1. 使用事件驱动的配置管理器
        auto& config_manager = config::EventDrivenConfigManager::instance();

        // 2. 注册配置属性
        auto gateway_config = std::make_shared<gateway::GatewayConfig>();
        config_manager.register_configuration_properties(gateway_config);

        // 3. 设置文件监控（Spring Boot Actuator风格）
        setup_file_watcher([&config_manager](const std::string& config_file) {
            SHIELD_LOG_INFO << "📂 Config file changed: " << config_file;

            // 触发热重载 (equivalent to POST /actuator/refresh)
            config_manager.reload_config(config_file);
        });

        // 4. 监听配置变更事件（类似Spring Boot的@EventListener）
        events::GlobalEventPublisher::listen<
            events::config::ConfigRefreshEvent>(
            [](const events::config::ConfigRefreshEvent& event) {
                SHIELD_LOG_INFO << "🔄 Handling config refresh event...";

                // 相当于Spring Boot中的@RefreshScope Bean重建
                refresh_scoped_beans();

                // 相当于@ConfigurationProperties重新绑定
                rebind_configuration_properties();

                SHIELD_LOG_INFO << "✅ Config refresh completed!";
            },
            false,  // 同步处理
            -10     // 高优先级，优先处理
        );
    }

private:
    void setup_file_watcher(std::function<void(const std::string&)> callback) {
        // 文件监控实现...
        SHIELD_LOG_INFO << "📁 File watcher setup completed";
    }

    void refresh_scoped_beans() {
        // 重新创建@RefreshScope标记的Bean
        SHIELD_LOG_INFO << "🔄 Refreshing scoped beans...";
    }

    void rebind_configuration_properties() {
        // 重新绑定@ConfigurationProperties
        SHIELD_LOG_INFO << "🔗 Rebinding configuration properties...";
    }
};

// 示例：事件驱动的微服务通信
class EventDrivenMicroserviceCommunication {
public:
    void demonstrate_service_events() {
        // 服务A监听服务B的就绪事件
        events::GlobalEventPublisher::listen<
            events::lifecycle::ServiceReadyEvent>([](const events::lifecycle::
                                                         ServiceReadyEvent&
                                                             event) {
            if (event.get_service_name() == "DatabaseService") {
                SHIELD_LOG_INFO
                    << "🗄️  Database service ready, starting data migration...";
                start_data_migration();
            }
        });

        // 监听配置变更，触发服务重配置
        events::GlobalEventPublisher::listen<
            events::config::ConfigRefreshEvent>(
            [](const events::config::ConfigRefreshEvent& event) {
                // 检查是否需要重新配置服务发现
                reconfigure_service_discovery();

                // 检查是否需要重新配置负载均衡
                reconfigure_load_balancer();
            },
            true  // 异步处理，避免阻塞配置刷新
        );
    }

private:
    static void start_data_migration() {
        SHIELD_LOG_INFO << "🔄 Starting data migration...";
    }

    static void reconfigure_service_discovery() {
        SHIELD_LOG_INFO << "🔍 Reconfiguring service discovery...";
    }

    static void reconfigure_load_balancer() {
        SHIELD_LOG_INFO << "⚖️  Reconfiguring load balancer...";
    }
};

}  // namespace shield::examples

/*
🎯 Spring Boot vs 我们的事件系统对比

Spring Boot:
```java
@Component
public class ConfigListener {
    @EventListener
    @Async
    public void handleConfigRefresh(RefreshEvent event) {
        // 处理配置刷新
    }

    @EventListener(condition = "#event.serviceName == 'gateway'")
    public void handleServiceReady(ServiceReadyEvent event) {
        // 条件监听
    }
}

@RestController
public class RefreshEndpoint {
    @PostMapping("/actuator/refresh")
    public void refresh() {
        publisher.publishEvent(new RefreshEvent());
    }
}
```

我们的实现:
```cpp
// 注册监听器
events::GlobalEventPublisher::listen<events::config::ConfigRefreshEvent>(
    [](const auto& event) {
        // 处理配置刷新
    },
    true  // 异步
);

// 发布事件
events::GlobalEventPublisher::emit<events::config::ConfigRefreshEvent>("source");
```

✅ 优势对比:
1. 类型安全 - 编译时检查事件类型
2. 零拷贝 - 智能指针传递事件
3. 异步支持 - 内置线程池处理
4. 优先级控制 - 支持执行顺序
5. 异常隔离 - 单个监听器异常不影响其他
6. 高性能 - C++原生性能优势
*/