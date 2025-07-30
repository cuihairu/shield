#pragma once

#include <any>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace shield::events {

// 1. 事件基类 (equivalent to Spring's ApplicationEvent)
class Event {
public:
    explicit Event(std::any source = {})
        : source_(std::move(source)),
          timestamp_(std::chrono::system_clock::now()) {}

    virtual ~Event() = default;

    const std::any& get_source() const { return source_; }
    auto get_timestamp() const { return timestamp_; }
    virtual std::string get_event_type() const = 0;

private:
    std::any source_;
    std::chrono::system_clock::time_point timestamp_;
};

// 2. 配置相关事件
namespace config {

// 配置刷新事件 (equivalent to Spring's RefreshEvent)
class ConfigRefreshEvent : public Event {
public:
    explicit ConfigRefreshEvent(std::any source = {})
        : Event(std::move(source)) {}
    std::string get_event_type() const override { return "ConfigRefreshEvent"; }
};

// 配置变更事件 (equivalent to Spring's EnvironmentChangeEvent)
template <typename ConfigType>
class ConfigChangeEvent : public Event {
public:
    ConfigChangeEvent(std::shared_ptr<ConfigType> old_config,
                      std::shared_ptr<ConfigType> new_config,
                      std::any source = {})
        : Event(std::move(source)),
          old_config_(std::move(old_config)),
          new_config_(std::move(new_config)) {}

    std::string get_event_type() const override {
        return std::string("ConfigChangeEvent<") + typeid(ConfigType).name() +
               ">";
    }

    std::shared_ptr<ConfigType> get_old_config() const { return old_config_; }
    std::shared_ptr<ConfigType> get_new_config() const { return new_config_; }

private:
    std::shared_ptr<ConfigType> old_config_;
    std::shared_ptr<ConfigType> new_config_;
};

// 配置属性绑定事件
template <typename PropertiesType>
class ConfigPropertiesBindEvent : public Event {
public:
    explicit ConfigPropertiesBindEvent(
        std::shared_ptr<PropertiesType> properties, std::any source = {})
        : Event(std::move(source)), properties_(std::move(properties)) {}

    std::string get_event_type() const override {
        return std::string("ConfigPropertiesBindEvent<") +
               typeid(PropertiesType).name() + ">";
    }

    std::shared_ptr<PropertiesType> get_properties() const {
        return properties_;
    }

private:
    std::shared_ptr<PropertiesType> properties_;
};

}  // namespace config

// 3. 应用生命周期事件
namespace lifecycle {

class ApplicationStartedEvent : public Event {
public:
    explicit ApplicationStartedEvent(std::any source = {})
        : Event(std::move(source)) {}
    std::string get_event_type() const override {
        return "ApplicationStartedEvent";
    }
};

class ApplicationStoppingEvent : public Event {
public:
    explicit ApplicationStoppingEvent(std::any source = {})
        : Event(std::move(source)) {}
    std::string get_event_type() const override {
        return "ApplicationStoppingEvent";
    }
};

class ServiceReadyEvent : public Event {
public:
    ServiceReadyEvent(const std::string& service_name, std::any source = {})
        : Event(std::move(source)), service_name_(service_name) {}

    std::string get_event_type() const override { return "ServiceReadyEvent"; }
    const std::string& get_service_name() const { return service_name_; }

private:
    std::string service_name_;
};

}  // namespace lifecycle

// 4. 事件监听器接口 (equivalent to Spring's ApplicationListener)
template <typename EventType>
class EventListener {
public:
    virtual ~EventListener() = default;
    virtual void on_event(const EventType& event) = 0;
    virtual bool supports_async() const { return false; }
    virtual int get_order() const { return 0; }  // 执行顺序，数字越小越先执行
};

// 5. 函数式事件监听器
template <typename EventType>
class FunctionalEventListener : public EventListener<EventType> {
public:
    using HandlerFunction = std::function<void(const EventType&)>;

    FunctionalEventListener(HandlerFunction handler, bool async = false,
                            int order = 0)
        : handler_(std::move(handler)), async_(async), order_(order) {}

    void on_event(const EventType& event) override { handler_(event); }

    bool supports_async() const override { return async_; }
    int get_order() const override { return order_; }

private:
    HandlerFunction handler_;
    bool async_;
    int order_;
};

// 6. 事件发布器接口 (equivalent to Spring's ApplicationEventPublisher)
class EventPublisher {
public:
    virtual ~EventPublisher() = default;

    // 发布事件
    virtual void publish_event(std::shared_ptr<Event> event) = 0;

    // 模板方法，便于使用
    template <typename EventType, typename... Args>
    void emit_event(Args&&... args) {
        auto event = std::make_shared<EventType>(std::forward<Args>(args)...);
        publish_event(event);
    }

    // 注册监听器
    template <typename EventType>
    void add_listener(std::shared_ptr<EventListener<EventType>> listener) {
        register_listener(std::type_index(typeid(EventType)),
                          std::static_pointer_cast<void>(listener));
    }

    // 函数式监听器注册
    template <typename EventType>
    void on(std::function<void(const EventType&)> handler, bool async = false,
            int order = 0) {
        auto listener = std::make_shared<FunctionalEventListener<EventType>>(
            std::move(handler), async, order);
        add_listener<EventType>(listener);
    }

protected:
    virtual void register_listener(std::type_index event_type,
                                   std::shared_ptr<void> listener) = 0;
};

}  // namespace shield::events