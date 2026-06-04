#define BOOST_TEST_MODULE ApplicationContextTest
#include <boost/test/unit_test.hpp>
#include <memory>
#include <stdexcept>

#include "shield/core/application_context.hpp"
#include "shield/core/service.hpp"

using namespace shield::core;

namespace {

class MockService : public Service {
public:
    explicit MockService(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    void on_init(ApplicationContext&) override { init_called_ = true; }
    void on_start() override { start_called_ = true; }
    void on_stop() override { stop_called_ = true; }

    bool init_called() const { return init_called_; }
    bool start_called() const { return start_called_; }
    bool stop_called() const { return stop_called_; }

private:
    std::string name_;
    bool init_called_ = false;
    bool start_called_ = false;
    bool stop_called_ = false;
};

class SimpleBean {
public:
    explicit SimpleBean(int value) : value_(value) {}
    int value() const { return value_; }

private:
    int value_;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(ApplicationContextTests)

BOOST_AUTO_TEST_CASE(TestRegisterAndGetBean) {
    auto& ctx = ApplicationContext::instance();
    ctx.register_bean<SimpleBean>("test_bean", 42);

    auto bean = ctx.get_bean<SimpleBean>("test_bean");
    BOOST_REQUIRE(bean);
    BOOST_CHECK_EQUAL(bean->value(), 42);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestRegisterBeanBySharedPtr) {
    auto& ctx = ApplicationContext::instance();
    auto bean = std::make_shared<SimpleBean>(99);
    ctx.register_bean<SimpleBean>("shared_bean", bean);

    auto retrieved = ctx.get_bean<SimpleBean>("shared_bean");
    BOOST_REQUIRE(retrieved);
    BOOST_CHECK_EQUAL(retrieved->value(), 99);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestRegisterNullBeanThrows) {
    auto& ctx = ApplicationContext::instance();
    std::shared_ptr<SimpleBean> null_bean;
    BOOST_CHECK_THROW(ctx.register_bean<SimpleBean>("null", null_bean),
                      std::invalid_argument);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestDuplicateBeanNameThrows) {
    auto& ctx = ApplicationContext::instance();
    ctx.register_bean<SimpleBean>("dup", 1);
    BOOST_CHECK_THROW(ctx.register_bean<SimpleBean>("dup", 2),
                      std::runtime_error);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestGetNonExistentBeanThrows) {
    auto& ctx = ApplicationContext::instance();
    BOOST_CHECK_THROW(ctx.get_bean<SimpleBean>("nonexistent"),
                      std::runtime_error);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestGetBeanByType) {
    auto& ctx = ApplicationContext::instance();
    ctx.register_bean<SimpleBean>("typed_bean", 77);

    auto bean = ctx.get_bean<SimpleBean>();
    BOOST_REQUIRE(bean);
    BOOST_CHECK_EQUAL(bean->value(), 77);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestGetBeanByTypeReturnsNullIfNotRegistered) {
    auto& ctx = ApplicationContext::instance();
    auto bean = ctx.get_bean<SimpleBean>();
    BOOST_CHECK(!bean);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestRegisterService) {
    auto& ctx = ApplicationContext::instance();
    auto svc = ctx.register_service<MockService>("mock_svc");

    BOOST_REQUIRE(svc);
    BOOST_CHECK_EQUAL(svc->name(), "mock_svc");

    auto retrieved = ctx.get_service<MockService>();
    BOOST_REQUIRE(retrieved);
    BOOST_CHECK_EQUAL(retrieved->name(), "mock_svc");

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestInitStartStopLifecycle) {
    auto& ctx = ApplicationContext::instance();
    auto svc = std::make_shared<MockService>("lifecycle_svc");
    ctx.register_service<MockService>("lifecycle_svc", svc);

    ctx.init_all();
    BOOST_CHECK(svc->init_called());

    ctx.start_all();
    BOOST_CHECK(svc->start_called());

    ctx.stop_all();
    BOOST_CHECK(svc->stop_called());

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestStopAllCallsInReverseOrder) {
    auto& ctx = ApplicationContext::instance();
    std::vector<std::string> stop_order;

    class OrderTracker : public Service {
    public:
        OrderTracker(std::string name, std::vector<std::string>& order)
            : name_(std::move(name)), order_(order) {}
        std::string name() const override { return name_; }
        void on_stop() override { order_.push_back(name_); }

    private:
        std::string name_;
        std::vector<std::string>& order_;
    };

    auto s1 = std::make_shared<OrderTracker>("first", stop_order);
    auto s2 = std::make_shared<OrderTracker>("second", stop_order);
    auto s3 = std::make_shared<OrderTracker>("third", stop_order);

    ctx.register_service<OrderTracker>("first", s1);
    ctx.register_service<OrderTracker>("second", s2);
    ctx.register_service<OrderTracker>("third", s3);

    ctx.stop_all();

    BOOST_REQUIRE_EQUAL(stop_order.size(), 3);
    BOOST_CHECK_EQUAL(stop_order[0], "third");
    BOOST_CHECK_EQUAL(stop_order[1], "second");
    BOOST_CHECK_EQUAL(stop_order[2], "first");

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestResetClearsEverything) {
    auto& ctx = ApplicationContext::instance();
    ctx.register_bean<SimpleBean>("to_clear", 1);
    ctx.reset();

    BOOST_CHECK(!ctx.get_bean<SimpleBean>());
}

BOOST_AUTO_TEST_SUITE_END()
