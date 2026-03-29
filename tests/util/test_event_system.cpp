#define BOOST_TEST_MODULE EventSystemTests
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "shield/events/event_publisher.hpp"
#include "shield/events/event_system.hpp"

namespace shield::events {

class TestEvent : public Event {
public:
    explicit TestEvent(int value = 0) : Event(), value_(value) {}

    std::string get_event_type() const override { return "TestEvent"; }
    int get_value() const { return value_; }
    void set_value(int value) { value_ = value; }

private:
    int value_;
};

class EventCounter {
public:
    int count = 0;
    std::string last_event_type;
    std::vector<std::string> event_types;

    void reset() {
        count = 0;
        last_event_type.clear();
        event_types.clear();
    }
};

BOOST_AUTO_TEST_SUITE(EventTests)

BOOST_AUTO_TEST_CASE(test_event_creation) {
    TestEvent event(42);

    BOOST_CHECK_EQUAL(event.get_event_type(), "TestEvent");
    BOOST_CHECK_EQUAL(event.get_value(), 42);
}

BOOST_AUTO_TEST_CASE(test_event_with_source) {
    std::string source = "test_source";
    TestEvent event(100);
    BOOST_CHECK_EQUAL(event.get_value(), 100);
}

BOOST_AUTO_TEST_CASE(test_event_timestamp) {
    auto before = std::chrono::system_clock::now();
    TestEvent event;
    auto after = std::chrono::system_clock::now();

    auto timestamp = event.get_timestamp();
    BOOST_CHECK(timestamp >= before);
    BOOST_CHECK(timestamp <= after);
}

BOOST_AUTO_TEST_CASE(test_event_value_modification) {
    TestEvent event(10);
    BOOST_CHECK_EQUAL(event.get_value(), 10);

    event.set_value(20);
    BOOST_CHECK_EQUAL(event.get_value(), 20);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ConfigEventTests)

BOOST_AUTO_TEST_CASE(test_config_refresh_event) {
    config::ConfigRefreshEvent event;

    BOOST_CHECK_EQUAL(event.get_event_type(), "ConfigRefreshEvent");
}

BOOST_AUTO_TEST_CASE(test_config_refresh_event_with_source) {
    std::string source = "config_manager";
    config::ConfigRefreshEvent event(std::any{source});

    BOOST_CHECK_EQUAL(event.get_event_type(), "ConfigRefreshEvent");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LifecycleEventTests)

BOOST_AUTO_TEST_CASE(test_application_started_event) {
    lifecycle::ApplicationStartedEvent event;

    BOOST_CHECK_EQUAL(event.get_event_type(), "ApplicationStartedEvent");
}

BOOST_AUTO_TEST_CASE(test_application_stopping_event) {
    lifecycle::ApplicationStoppingEvent event;

    BOOST_CHECK_EQUAL(event.get_event_type(), "ApplicationStoppingEvent");
}

BOOST_AUTO_TEST_CASE(test_service_ready_event) {
    lifecycle::ServiceReadyEvent event("test_service");

    BOOST_CHECK_EQUAL(event.get_event_type(), "ServiceReadyEvent");
    BOOST_CHECK_EQUAL(event.get_service_name(), "test_service");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EventListenerTests)

BOOST_AUTO_TEST_CASE(test_functional_event_listener) {
    EventCounter counter;
    auto listener = std::make_shared<FunctionalEventListener<TestEvent>>(
        [&counter](const TestEvent& event) {
            counter.count++;
            counter.last_event_type = event.get_event_type();
            counter.event_types.push_back(event.get_event_type());
        });

    TestEvent event(42);
    listener->on_event(event);

    BOOST_CHECK_EQUAL(counter.count, 1);
    BOOST_CHECK_EQUAL(counter.last_event_type, "TestEvent");
}

BOOST_AUTO_TEST_CASE(test_listener_async_support) {
    auto listener_async = std::make_shared<FunctionalEventListener<TestEvent>>(
        [](const TestEvent&) {}, true);

    auto listener_sync = std::make_shared<FunctionalEventListener<TestEvent>>(
        [](const TestEvent&) {}, false);

    BOOST_CHECK(listener_async->supports_async());
    BOOST_CHECK(!listener_sync->supports_async());
}

BOOST_AUTO_TEST_CASE(test_listener_order) {
    auto listener1 = std::make_shared<FunctionalEventListener<TestEvent>>(
        [](const TestEvent&) {}, false, 1);

    auto listener2 = std::make_shared<FunctionalEventListener<TestEvent>>(
        [](const TestEvent&) {}, false, 2);

    auto listener3 = std::make_shared<FunctionalEventListener<TestEvent>>(
        [](const TestEvent&) {}, false, -1);

    BOOST_CHECK_EQUAL(listener1->get_order(), 1);
    BOOST_CHECK_EQUAL(listener2->get_order(), 2);
    BOOST_CHECK_EQUAL(listener3->get_order(), -1);
}

BOOST_AUTO_TEST_CASE(test_multiple_events_to_same_listener) {
    EventCounter counter;
    auto listener = std::make_shared<FunctionalEventListener<TestEvent>>(
        [&counter](const TestEvent& event) {
            counter.count++;
            counter.event_types.push_back(event.get_event_type());
        });

    TestEvent event1(1);
    TestEvent event2(2);
    TestEvent event3(3);

    listener->on_event(event1);
    listener->on_event(event2);
    listener->on_event(event3);

    BOOST_CHECK_EQUAL(counter.count, 3);
    BOOST_CHECK_EQUAL(counter.event_types.size(), 3);
}

BOOST_AUTO_TEST_SUITE_END()

class CounterEventListener : public EventListener<TestEvent> {
public:
    int count = 0;
    int last_value = 0;

    void on_event(const TestEvent& event) override {
        count++;
        last_value = event.get_value();
    }

    bool supports_async() const override { return false; }
    int get_order() const override { return 0; }
};

BOOST_AUTO_TEST_SUITE(CustomEventListenerTests)

BOOST_AUTO_TEST_CASE(test_custom_listener) {
    auto listener = std::make_shared<CounterEventListener>();

    TestEvent event(42);
    listener->on_event(event);

    BOOST_CHECK_EQUAL(listener->count, 1);
    BOOST_CHECK_EQUAL(listener->last_value, 42);
}

BOOST_AUTO_TEST_CASE(test_custom_listener_multiple_events) {
    auto listener = std::make_shared<CounterEventListener>();

    listener->on_event(TestEvent(10));
    listener->on_event(TestEvent(20));
    listener->on_event(TestEvent(30));

    BOOST_CHECK_EQUAL(listener->count, 3);
    BOOST_CHECK_EQUAL(listener->last_value, 30);
}

BOOST_AUTO_TEST_SUITE_END()

class MockEventPublisher : public EventPublisher {
public:
    struct ListenerInfo {
        ListenerRegistration registration;
    };

    std::vector<std::shared_ptr<Event>> published_events;
    std::unordered_map<std::type_index, std::vector<ListenerInfo>>
        listeners_map;

    void publish_event(std::shared_ptr<Event> event) override {
        if (!event) {
            return;
        }
        published_events.push_back(event);

        Event* event_ptr = event.get();
        const std::type_index type_idx(typeid(*event_ptr));

        std::vector<ListenerInfo> listeners;
        auto add = [&](const std::type_index& key) {
            auto it = listeners_map.find(key);
            if (it == listeners_map.end()) {
                return;
            }
            listeners.insert(listeners.end(), it->second.begin(),
                             it->second.end());
        };

        add(type_idx);
        if (type_idx != std::type_index(typeid(Event))) {
            add(std::type_index(typeid(Event)));
        }

        std::sort(listeners.begin(), listeners.end(),
                  [](const ListenerInfo& a, const ListenerInfo& b) {
                      return a.registration.order < b.registration.order;
                  });

        for (auto& info : listeners) {
            info.registration.invoke(event);
        }
    }

protected:
    void register_listener(std::type_index event_type,
                           ListenerRegistration registration) override {
        listeners_map[event_type].push_back({std::move(registration)});
    }
};

BOOST_AUTO_TEST_SUITE(EventPublisherTests)

BOOST_AUTO_TEST_CASE(test_publish_event) {
    MockEventPublisher publisher;
    auto event = std::make_shared<TestEvent>(123);

    publisher.publish_event(event);

    BOOST_CHECK_EQUAL(publisher.published_events.size(), 1);
    BOOST_CHECK_EQUAL(
        std::dynamic_pointer_cast<TestEvent>(publisher.published_events[0])
            ->get_value(),
        123);
}

BOOST_AUTO_TEST_CASE(test_emit_event_template) {
    MockEventPublisher publisher;

    publisher.emit_event<TestEvent>(456);

    BOOST_CHECK_EQUAL(publisher.published_events.size(), 1);
}

BOOST_AUTO_TEST_CASE(test_add_listener) {
    MockEventPublisher publisher;
    EventCounter counter;

    auto listener = std::make_shared<FunctionalEventListener<TestEvent>>(
        [&counter](const TestEvent& event) {
            counter.count++;
            counter.last_event_type = event.get_event_type();
        });

    publisher.add_listener<TestEvent>(listener);
    publisher.emit_event<TestEvent>(789);

    BOOST_CHECK_EQUAL(counter.count, 1);
    BOOST_CHECK_EQUAL(counter.last_event_type, "TestEvent");
}

BOOST_AUTO_TEST_CASE(test_on_functional_listener) {
    MockEventPublisher publisher;
    int call_count = 0;
    int last_value = 0;

    publisher.on<TestEvent>(
        [&call_count, &last_value](const TestEvent& event) {
            call_count++;
            last_value = event.get_value();
        });

    publisher.emit_event<TestEvent>(100);
    publisher.emit_event<TestEvent>(200);

    BOOST_CHECK_EQUAL(call_count, 2);
    BOOST_CHECK_EQUAL(last_value, 200);
}

BOOST_AUTO_TEST_CASE(test_multiple_listeners) {
    MockEventPublisher publisher;
    int count1 = 0, count2 = 0, count3 = 0;

    publisher.on<TestEvent>([&count1](const TestEvent&) { count1++; });
    publisher.on<TestEvent>([&count2](const TestEvent&) { count2++; });
    publisher.on<TestEvent>([&count3](const TestEvent&) { count3++; });

    publisher.emit_event<TestEvent>(1);

    BOOST_CHECK_EQUAL(count1, 1);
    BOOST_CHECK_EQUAL(count2, 1);
    BOOST_CHECK_EQUAL(count3, 1);
}

BOOST_AUTO_TEST_CASE(test_listener_order) {
    MockEventPublisher publisher;
    std::vector<int> execution_order;

    publisher.on<TestEvent>(
        [&execution_order](const TestEvent&) { execution_order.push_back(2); },
        false, 2);

    publisher.on<TestEvent>(
        [&execution_order](const TestEvent&) { execution_order.push_back(1); },
        false, 1);

    publisher.on<TestEvent>(
        [&execution_order](const TestEvent&) { execution_order.push_back(3); },
        false, 3);

    publisher.emit_event<TestEvent>(0);

    BOOST_CHECK_EQUAL(execution_order.size(), 3);
    BOOST_CHECK_EQUAL(execution_order[0], 1);
    BOOST_CHECK_EQUAL(execution_order[1], 2);
    BOOST_CHECK_EQUAL(execution_order[2], 3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ConfigChangeEventTests)

struct TestConfig {
    int value = 0;
    std::string name;
};

BOOST_AUTO_TEST_CASE(test_config_change_event) {
    auto old_config = std::make_shared<TestConfig>();
    old_config->value = 100;
    old_config->name = "old";

    auto new_config = std::make_shared<TestConfig>();
    new_config->value = 200;
    new_config->name = "new";

    config::ConfigChangeEvent<TestConfig> event(old_config, new_config);

    BOOST_CHECK(event.get_event_type().find("ConfigChangeEvent") !=
                std::string::npos);
    BOOST_CHECK_EQUAL(event.get_old_config()->value, 100);
    BOOST_CHECK_EQUAL(event.get_new_config()->value, 200);
    BOOST_CHECK_EQUAL(event.get_old_config()->name, "old");
    BOOST_CHECK_EQUAL(event.get_new_config()->name, "new");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(DefaultEventPublisherTests)

BOOST_AUTO_TEST_CASE(test_default_publisher_sync_listener) {
    DefaultEventPublisher publisher;
    int received_value = 0;
    int receive_count = 0;

    publisher.on<TestEvent>([&](const TestEvent& event) {
        received_value = event.get_value();
        receive_count++;
    });

    publisher.emit_event<TestEvent>(77);

    BOOST_CHECK_EQUAL(receive_count, 1);
    BOOST_CHECK_EQUAL(received_value, 77);
}

BOOST_AUTO_TEST_CASE(test_default_publisher_async_listener) {
    DefaultEventPublisher publisher;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int received_value = 0;

    publisher.on<TestEvent>(
        [&](const TestEvent& event) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                received_value = event.get_value();
                done = true;
            }
            cv.notify_one();
        },
        true);

    publisher.emit_event<TestEvent>(88);

    std::unique_lock<std::mutex> lock(mutex);
    const bool completed =
        cv.wait_for(lock, std::chrono::milliseconds(500),
                    [&] { return done; });

    BOOST_CHECK(completed);
    BOOST_CHECK_EQUAL(received_value, 88);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(GlobalEventPublisherTests)

class DispatchEvent : public Event {
public:
    explicit DispatchEvent(int value) : Event(), value_(value) {}
    std::string get_event_type() const override { return "DispatchEvent"; }
    int value() const { return value_; }

private:
    int value_;
};

BOOST_AUTO_TEST_CASE(test_sync_listener_dispatch) {
    std::atomic<int> received{0};

    GlobalEventPublisher::listen<DispatchEvent>(
        [&received](const DispatchEvent& event) { received = event.value(); });

    GlobalEventPublisher::emit<DispatchEvent>(123);
    BOOST_CHECK_EQUAL(received.load(), 123);
}

class OrderEvent : public Event {
public:
    OrderEvent() : Event() {}
    std::string get_event_type() const override { return "OrderEvent"; }
};

BOOST_AUTO_TEST_CASE(test_listener_ordering) {
    std::mutex mutex;
    std::vector<int> order;

    GlobalEventPublisher::listen<OrderEvent>(
        [&mutex, &order](const OrderEvent&) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(2);
        },
        false, 2);

    GlobalEventPublisher::listen<OrderEvent>(
        [&mutex, &order](const OrderEvent&) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(1);
        },
        false, 1);

    GlobalEventPublisher::emit<OrderEvent>();

    std::lock_guard<std::mutex> lock(mutex);
    BOOST_REQUIRE_EQUAL(order.size(), 2U);
    BOOST_CHECK_EQUAL(order[0], 1);
    BOOST_CHECK_EQUAL(order[1], 2);
}

class AsyncEvent : public Event {
public:
    AsyncEvent() : Event() {}
    std::string get_event_type() const override { return "AsyncEvent"; }
};

BOOST_AUTO_TEST_CASE(test_async_listener_dispatch) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    GlobalEventPublisher::listen<AsyncEvent>(
        [&mutex, &cv, &done](const AsyncEvent&) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            cv.notify_one();
        },
        true, 0);

    GlobalEventPublisher::emit<AsyncEvent>();

    std::unique_lock<std::mutex> lock(mutex);
    const auto ok = cv.wait_for(lock, std::chrono::seconds(2),
                                [&done] { return done; });
    BOOST_CHECK(ok);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::events
