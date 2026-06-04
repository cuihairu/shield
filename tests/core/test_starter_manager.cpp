#define BOOST_TEST_MODULE StarterManagerTest
#include <boost/test/unit_test.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "shield/core/application_context.hpp"
#include "shield/core/starter_manager.hpp"

using namespace shield::core;

namespace {

class MockStarter : public IStarter {
public:
    explicit MockStarter(std::string name,
                         std::vector<std::string> deps = {},
                         bool enabled = true)
        : name_(std::move(name)),
          deps_(std::move(deps)),
          enabled_(enabled) {}

    std::string name() const override { return name_; }
    std::vector<std::string> depends_on() const override { return deps_; }
    bool is_enabled() const override { return enabled_; }

    void initialize(ApplicationContext&) override { initialized_ = true; }
    void pre_initialize(ApplicationContext&) override { pre_initialized_ = true; }
    void post_initialize(ApplicationContext&) override {
        post_initialized_ = true;
    }

    bool initialized() const { return initialized_; }
    bool pre_initialized() const { return pre_initialized_; }
    bool post_initialized() const { return post_initialized_; }

private:
    std::string name_;
    std::vector<std::string> deps_;
    bool enabled_;
    bool initialized_ = false;
    bool pre_initialized_ = false;
    bool post_initialized_ = false;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(StarterManagerTests)

BOOST_AUTO_TEST_CASE(TestRegisterStarter) {
    StarterManager mgr;
    auto starter = std::make_unique<MockStarter>("test_starter");
    auto* raw = starter.get();

    mgr.register_starter(std::move(starter));
    BOOST_CHECK(mgr.has_starter("test_starter"));

    (void)raw;
}

BOOST_AUTO_TEST_CASE(TestRegisterNullStarterThrows) {
    StarterManager mgr;
    BOOST_CHECK_THROW(mgr.register_starter(nullptr), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestDuplicateStarterThrows) {
    StarterManager mgr;
    mgr.register_starter(std::make_unique<MockStarter>("dup"));
    BOOST_CHECK_THROW(mgr.register_starter(std::make_unique<MockStarter>("dup")),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TestHasStarter) {
    StarterManager mgr;
    BOOST_CHECK(!mgr.has_starter("missing"));
    mgr.register_starter(std::make_unique<MockStarter>("exists"));
    BOOST_CHECK(mgr.has_starter("exists"));
}

BOOST_AUTO_TEST_CASE(TestInitializeAllCallsLifecycle) {
    StarterManager mgr;
    auto starter = std::make_unique<MockStarter>("lifecycle");
    auto* raw = starter.get();
    mgr.register_starter(std::move(starter));

    auto& ctx = ApplicationContext::instance();
    mgr.initialize_all(ctx);

    BOOST_CHECK(raw->pre_initialized());
    BOOST_CHECK(raw->initialized());
    BOOST_CHECK(raw->post_initialized());

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestDisabledStarterSkipped) {
    StarterManager mgr;
    auto starter = std::make_unique<MockStarter>("disabled", std::vector<std::string>{}, false);
    auto* raw = starter.get();
    mgr.register_starter(std::move(starter));

    auto& ctx = ApplicationContext::instance();
    mgr.initialize_all(ctx);

    BOOST_CHECK(!raw->initialized());

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestDependencyOrder) {
    StarterManager mgr;
    std::vector<std::string> init_order;

    class OrderTracker : public IStarter {
    public:
        OrderTracker(std::string name, std::vector<std::string> deps,
                     std::vector<std::string>& order)
            : name_(std::move(name)),
              deps_(std::move(deps)),
              order_(order) {}

        std::string name() const override { return name_; }
        std::vector<std::string> depends_on() const override { return deps_; }
        void initialize(ApplicationContext&) override {
            order_.push_back(name_);
        }

    private:
        std::string name_;
        std::vector<std::string> deps_;
        std::vector<std::string>& order_;
    };

    mgr.register_starter(
        std::make_unique<OrderTracker>("B", std::vector<std::string>{"A"}, init_order));
    mgr.register_starter(
        std::make_unique<OrderTracker>("C", std::vector<std::string>{"B"}, init_order));
    mgr.register_starter(
        std::make_unique<OrderTracker>("A", std::vector<std::string>{}, init_order));

    auto& ctx = ApplicationContext::instance();
    mgr.initialize_all(ctx);

    // A must be initialized before B, B before C
    BOOST_REQUIRE_EQUAL(init_order.size(), 3);
    BOOST_CHECK_EQUAL(init_order[0], "A");
    BOOST_CHECK_EQUAL(init_order[1], "B");
    BOOST_CHECK_EQUAL(init_order[2], "C");

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestCircularDependencyThrows) {
    StarterManager mgr;
    mgr.register_starter(
        std::make_unique<MockStarter>("A", std::vector<std::string>{"B"}));
    mgr.register_starter(
        std::make_unique<MockStarter>("B", std::vector<std::string>{"A"}));

    auto& ctx = ApplicationContext::instance();
    BOOST_CHECK_THROW(mgr.initialize_all(ctx), std::runtime_error);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestMissingDependencyThrows) {
    StarterManager mgr;
    mgr.register_starter(
        std::make_unique<MockStarter>("A", std::vector<std::string>{"nonexistent"}));

    auto& ctx = ApplicationContext::instance();
    BOOST_CHECK_THROW(mgr.initialize_all(ctx), std::runtime_error);

    ctx.reset();
}

BOOST_AUTO_TEST_CASE(TestEmptyManagerDoesNothing) {
    StarterManager mgr;
    auto& ctx = ApplicationContext::instance();
    BOOST_CHECK_NO_THROW(mgr.initialize_all(ctx));

    ctx.reset();
}

BOOST_AUTO_TEST_SUITE_END()
