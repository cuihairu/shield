// shield/tests/di/test_container.cpp
#define BOOST_TEST_MODULE container_tests
#include <boost/test/unit_test.hpp>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "shield/di/container.hpp"

using namespace shield::di;

// =====================================
// 测试接口和实现类
// =====================================

struct IService {
    virtual ~IService() = default;
    virtual std::string get_name() const = 0;
};

class ServiceA : public IService {
public:
    std::string get_name() const override { return "ServiceA"; }
};

class ServiceB : public IService {
public:
    std::string get_name() const override { return "ServiceB"; }
};

struct IRepository {
    virtual ~IRepository() = default;
    virtual bool save(const std::string& data) = 0;
};

class RepositoryImpl : public IRepository {
public:
    bool save(const std::string& data) override {
        saved_data = data;
        return true;
    }

    std::string saved_data;
};

class SingletonService {
public:
    static int instance_count;
    SingletonService() { instance_count++; }
    int get_value() const { return 42; }
};

int SingletonService::instance_count = 0;

// =====================================
// Container 基础功能测试
// =====================================

BOOST_AUTO_TEST_SUITE(container_suite)

BOOST_AUTO_TEST_CASE(test_container_construction) {
    Container container;

    BOOST_CHECK_EQUAL(container.service_count(), 0);
    BOOST_CHECK(!container.is_registered<IService>());
}

BOOST_AUTO_TEST_CASE(test_container_add_transient) {
    Container container;

    container.add_transient<IService, ServiceA>();

    BOOST_CHECK(container.is_registered<IService>());
    BOOST_CHECK_EQUAL(container.service_count(), 1);
}

BOOST_AUTO_TEST_CASE(test_container_add_singleton) {
    Container container;

    container.add_singleton<IService, ServiceA>();

    BOOST_CHECK(container.is_registered<IService>());
    BOOST_CHECK_EQUAL(container.service_count(), 1);
}

BOOST_AUTO_TEST_CASE(test_container_add_instance) {
    Container container;

    auto service = std::make_shared<ServiceA>();
    container.add_instance<IService>(service);

    BOOST_CHECK(container.is_registered<IService>());

    auto retrieved = container.get_service<IService>();
    BOOST_CHECK_EQUAL(retrieved.get(), service.get());
}

BOOST_AUTO_TEST_CASE(test_container_add_factory) {
    Container container;

    container.add_factory<IService>(
        []() { return std::make_shared<ServiceB>(); },
        ServiceLifetime::TRANSIENT);

    BOOST_CHECK(container.is_registered<IService>());

    auto service = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service->get_name(), "ServiceB");
}

// =====================================
// Transient 生命周期测试
// =====================================

BOOST_AUTO_TEST_CASE(test_transient_returns_new_instances) {
    Container container;

    container.add_transient<IService, ServiceA>();

    auto service1 = container.get_service<IService>();
    auto service2 = container.get_service<IService>();

    BOOST_CHECK_NE(service1.get(), service2.get());
}

BOOST_AUTO_TEST_CASE(test_transient_multiple_types) {
    Container container;

    container.add_transient<IService, ServiceA>();
    container.add_transient<IRepository, RepositoryImpl>();

    auto service = container.get_service<IService>();
    auto repository = container.get_service<IRepository>();

    BOOST_CHECK_EQUAL(service->get_name(), "ServiceA");
    BOOST_CHECK(repository->save("test data"));
}

// =====================================
// Singleton 生命周期测试
// =====================================

BOOST_AUTO_TEST_CASE(test_singleton_returns_same_instance) {
    Container container;

    container.add_singleton<IService, ServiceA>();

    auto service1 = container.get_service<IService>();
    auto service2 = container.get_service<IService>();

    BOOST_CHECK_EQUAL(service1.get(), service2.get());
}

BOOST_AUTO_TEST_CASE(test_singleton_instance_count) {
    Container container;

    SingletonService::instance_count = 0;

    container.add_singleton<SingletonService, SingletonService>();

    auto service1 = container.get_service<SingletonService>();
    auto service2 = container.get_service<SingletonService>();

    BOOST_CHECK_EQUAL(service1.get(), service2.get());
    BOOST_CHECK_EQUAL(SingletonService::instance_count, 1);
}

BOOST_AUTO_TEST_CASE(test_singleton_with_factory) {
    Container container;

    int create_count = 0;

    container.add_factory<IService>(
        [&create_count]() {
            create_count++;
            return std::make_shared<ServiceA>();
        },
        ServiceLifetime::SINGLETON);

    auto service1 = container.get_service<IService>();
    auto service2 = container.get_service<IService>();

    BOOST_CHECK_EQUAL(service1.get(), service2.get());
    BOOST_CHECK_EQUAL(create_count, 1);
}

// =====================================
// 容器操作测试
// =====================================

BOOST_AUTO_TEST_CASE(test_container_clear) {
    Container container;

    container.add_singleton<IService, ServiceA>();
    container.add_transient<IRepository, RepositoryImpl>();

    BOOST_CHECK_EQUAL(container.service_count(), 2);

    container.clear();

    BOOST_CHECK_EQUAL(container.service_count(), 0);
    BOOST_CHECK(!container.is_registered<IService>());
}

BOOST_AUTO_TEST_CASE(test_container_move_semantics) {
    Container container1;

    container1.add_singleton<IService, ServiceA>();

    Container container2 = std::move(container1);

    BOOST_CHECK(container2.is_registered<IService>());
    BOOST_CHECK_EQUAL(container2.service_count(), 1);

    auto service = container2.get_service<IService>();
    BOOST_CHECK_EQUAL(service->get_name(), "ServiceA");
}

BOOST_AUTO_TEST_CASE(test_container_not_copyable) {
    // Container 不应可拷贝
    // 这只是一个编译时检查，如果可以编译则说明有问题
    // Container container1;
    // Container container2 = container1;  // 这行不应该编译
}

// =====================================
// 错误处理测试
// =====================================

BOOST_AUTO_TEST_CASE(test_get_unregistered_service_throws) {
    Container container;

    BOOST_CHECK_THROW(container.get_service<IService>(),
                     std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_get_unregistered_service_error_message) {
    Container container;

    try {
        container.get_service<IService>();
        BOOST_FAIL("Should have thrown an exception");
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        BOOST_CHECK(msg.find("not registered") != std::string::npos);
    }
}

// =====================================
// 复杂场景测试
// =====================================

BOOST_AUTO_TEST_CASE(test_container_override_registration) {
    Container container;

    container.add_singleton<IService, ServiceA>();

    auto service1 = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service1->get_name(), "ServiceA");

    // 覆盖注册
    container.add_singleton<IService, ServiceB>();

    auto service2 = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service2->get_name(), "ServiceB");

    // 应该是不同的实例
    BOOST_CHECK_NE(service1.get(), service2.get());
}

BOOST_AUTO_TEST_CASE(test_container_mixed_lifetimes) {
    Container container;

    container.add_singleton<IService, ServiceA>();
    container.add_transient<IRepository, RepositoryImpl>();

    // Singleton
    auto service1 = container.get_service<IService>();
    auto service2 = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service1.get(), service2.get());

    // Transient
    auto repo1 = container.get_service<IRepository>();
    auto repo2 = container.get_service<IRepository>();
    BOOST_CHECK_NE(repo1.get(), repo2.get());
}

BOOST_AUTO_TEST_CASE(test_container_factory_with_captured_state) {
    Container container;

    int counter = 0;

    container.add_factory<IService>(
        [&counter]() {
            counter++;
            return std::make_shared<ServiceB>();
        },
        ServiceLifetime::TRANSIENT);

    auto service1 = container.get_service<IService>();
    auto service2 = container.get_service<IService>();

    BOOST_CHECK_EQUAL(counter, 2);
    BOOST_CHECK_NE(service1.get(), service2.get());
}

// =====================================
// 自定义删除器和清理测试
// =====================================

class ResourceService : public IService {
public:
    std::string get_name() const override { return "ResourceService"; }

    bool is_released() const { return released_; }
    void release() { released_ = true; }

private:
    bool released_ = false;
};

BOOST_AUTO_TEST_CASE(test_container_with_cleanup) {
    Container container;

    auto resource = std::make_shared<ResourceService>();
    container.add_instance<IService>(resource);

    auto service = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service->get_name(), "ResourceService");

    // 模拟清理
    auto resource_service = std::dynamic_pointer_cast<ResourceService>(service);
    resource_service->release();

    BOOST_CHECK(resource_service->is_released());
}

// =====================================
// 线程安全测试
// =====================================

BOOST_AUTO_TEST_CASE(test_container_thread_safe_singleton_access) {
    Container container;

    SingletonService::instance_count = 0;
    container.add_singleton<SingletonService, SingletonService>();

    const int NUM_THREADS = 10;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&container]() {
            for (int j = 0; j < 100; ++j) {
                auto service = container.get_service<SingletonService>();
                BOOST_CHECK_EQUAL(service->get_value(), 42);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 应该只创建一个实例
    BOOST_CHECK_EQUAL(SingletonService::instance_count, 1);
}

// =====================================
// 服务层次结构测试
// =====================================

struct ILogger {
    virtual ~ILogger() = default;
    virtual void log(const std::string& message) = 0;
};

class ConsoleLogger : public ILogger {
public:
    void log(const std::string& message) override {
        logged_messages.push_back(message);
    }

    std::vector<std::string> logged_messages;
};

struct IDataService {
    virtual ~IDataService() = default;
    virtual void process(const std::string& data) = 0;
};

class DataServiceImpl : public IDataService {
public:
    std::shared_ptr<ILogger> logger;

    DataServiceImpl(std::shared_ptr<ILogger> l) : logger(l) {}

    void process(const std::string& data) override {
        logger->log("Processing: " + data);
    }
};

BOOST_AUTO_TEST_CASE(test_container_service_hierarchy) {
    Container container;

    container.add_singleton<ILogger, ConsoleLogger>();

    // 注意: 当前实现不支持构造函数注入，需要手动设置依赖
    container.add_factory<IDataService>(
        [&container]() {
            auto logger = container.get_service<ILogger>();
            return std::make_shared<DataServiceImpl>(logger);
        },
        ServiceLifetime::TRANSIENT);

    auto data_service = container.get_service<IDataService>();
    data_service->process("test data");

    auto logger = container.get_service<ILogger>();
    auto console_logger = std::dynamic_pointer_cast<ConsoleLogger>(logger);

    BOOST_REQUIRE(console_logger);
    BOOST_CHECK_EQUAL(console_logger->logged_messages.size(), 1);
    BOOST_CHECK_EQUAL(console_logger->logged_messages[0], "Processing: test data");
}

// =====================================
// 条件注册测试
// =====================================

BOOST_AUTO_TEST_CASE(test_container_conditional_registration) {
    Container container;

    bool use_service_a = true;

    if (use_service_a) {
        container.add_singleton<IService, ServiceA>();
    } else {
        container.add_singleton<IService, ServiceB>();
    }

    auto service = container.get_service<IService>();
    BOOST_CHECK_EQUAL(service->get_name(), "ServiceA");
}

// =====================================
// 容器统计和查询测试
// =====================================

BOOST_AUTO_TEST_CASE(test_container_query_multiple_services) {
    Container container;

    container.add_singleton<IService, ServiceA>();
    container.add_transient<IRepository, RepositoryImpl>();

    BOOST_CHECK(container.is_registered<IService>());
    BOOST_CHECK(container.is_registered<IRepository>());
    BOOST_CHECK(!container.is_registered<SingletonService>());

    BOOST_CHECK_EQUAL(container.service_count(), 2);
}

BOOST_AUTO_TEST_SUITE_END()
