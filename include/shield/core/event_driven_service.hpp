#pragma once

#include "shield/config/event_driven_config_manager.hpp"
#include "shield/core/service.hpp"
#include "shield/events/event_publisher.hpp"
#include "shield/events/event_system.hpp"

namespace shield::core {

// 事件驱动的Service基类
class EventDrivenService : public Service {
public:
    explicit EventDrivenService(const std::string& name)
        : service_name_(name) {}

    void on_init(ApplicationContext& ctx) override {
        Service::on_init(ctx);
        register_event_listeners();
    }

    std::string name() const override { return service_name_; }

protected:
    // 子类可以重写此方法来注册事件监听器
    virtual void register_event_listeners() {}

    // 便捷的事件监听注册方法
    template <typename EventType>
    void listen_to(std::function<void(const EventType&)> handler,
                   bool async = false, int order = 0) {
        events::GlobalEventPublisher::listen<EventType>(
            [this, handler](const EventType& event) {
                try {
                    handler(event);
                } catch (const std::exception& e) {
                    SHIELD_LOG_ERROR << "Exception in " << name()
                                     << " event handler: " << e.what();
                }
            },
            async, order);
    }

private:
    std::string service_name_;
};

// 可重载配置的Service (使用事件系统)
class EventDrivenReloadableService : public EventDrivenService,
                                     public IReloadableService {
public:
    explicit EventDrivenReloadableService(const std::string& name)
        : EventDrivenService(name) {}

protected:
    void register_event_listeners() override {
        EventDrivenService::register_event_listeners();

        // 监听通用配置刷新事件
        listen_to<events::config::ConfigRefreshEvent>(
            [this](const events::config::ConfigRefreshEvent& event) {
                on_config_refresh_event(event);
            },
            false, 0);
    }

    // 处理配置刷新事件
    virtual void on_config_refresh_event(
        const events::config::ConfigRefreshEvent& event) {
        // 默认调用传统的配置重载方法
        on_config_reloaded();
    }

    // 传统的配置重载接口，子类可以继续使用
    void on_config_reloaded() override {
        SHIELD_LOG_INFO << name() << " received config reload signal";
    }
};

}  // namespace shield::core

// 使用示例：更新GatewayService使用事件系统
namespace shield::gateway {

class EventDrivenGatewayService : public core::EventDrivenReloadableService {
public:
    EventDrivenGatewayService(const std::string& name,
                              actor::DistributedActorSystem& actor_system,
                              script::LuaVMPool& lua_vm_pool,
                              std::shared_ptr<GatewayConfig> config)
        : EventDrivenReloadableService(name),
          m_actor_system(actor_system),
          m_lua_vm_pool(lua_vm_pool),
          m_config(config) {}

protected:
    void register_event_listeners() override {
        core::EventDrivenReloadableService::register_event_listeners();

        // 监听特定的Gateway配置变更事件
        listen_to<events::config::ConfigPropertiesBindEvent<GatewayConfig>>(
            [this](
                const events::config::ConfigPropertiesBindEvent<GatewayConfig>&
                    event) {
                SHIELD_LOG_INFO << "Gateway received new config binding";
                m_config = event.get_properties();
                apply_new_gateway_config();
            },
            false, 0);

        // 监听应用启动完成事件
        listen_to<events::lifecycle::ApplicationStartedEvent>(
            [this](const events::lifecycle::ApplicationStartedEvent& event) {
                SHIELD_LOG_INFO
                    << "Gateway service detected application started";
                // 可以在这里做一些应用启动后的初始化工作
            },
            true, 0);  // 异步处理
    }

    void on_config_refresh_event(
        const events::config::ConfigRefreshEvent& event) override {
        SHIELD_LOG_INFO
            << "Gateway service handling config refresh event from: "
            << std::any_cast<std::string>(event.get_source());

        // 获取最新的配置
        auto& config_manager = config::EventDrivenConfigManager::instance();
        auto new_config =
            config_manager.get_configuration_properties<GatewayConfig>();

        if (new_config && new_config.get() != m_config.get()) {
            SHIELD_LOG_INFO << "Applying new Gateway configuration";
            m_config = new_config;
            apply_new_gateway_config();
        }
    }

private:
    void apply_new_gateway_config() {
        // 应用新的网关配置
        SHIELD_LOG_INFO << "Restarting Gateway with new configuration";

        // 发布服务状态变更事件
        events::GlobalEventPublisher::emit<
            events::lifecycle::ServiceReadyEvent>(
            name(), std::string("GatewayService"));

        // 重启网关逻辑...
        on_stop();
        on_start();
    }

    actor::DistributedActorSystem& m_actor_system;
    script::LuaVMPool& m_lua_vm_pool;
    std::shared_ptr<GatewayConfig> m_config;
};

}  // namespace shield::gateway