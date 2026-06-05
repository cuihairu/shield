#define BOOST_TEST_MODULE ServiceContainerTest
#include <boost/test/unit_test.hpp>

#include "shield/di/service_container.hpp"

using namespace shield::di;

// Test types
struct IMyService {
    virtual ~IMyService() = default;
    virtual std::string greet() const = 0;
};

class MyServiceImpl : public IMyService {
public:
    std::string greet() const override { return "hello"; }
};

class SimpleService {
public:
    int value() const { return 99; }
};

BOOST_AUTO_TEST_SUITE(ServiceContainerTests)

BOOST_AUTO_TEST_CASE(TestAddTransientAuto) {
    ServiceContainer container;
    container.add_transient_auto<IMyService, MyServiceImpl>();

    auto s1 = container.get_service<IMyService>();
    auto s2 = container.get_service<IMyService>();

    BOOST_CHECK(s1 != nullptr);
    BOOST_CHECK(s2 != nullptr);
    BOOST_CHECK_EQUAL(s1->greet(), "hello");
    // Transient should return different instances
    BOOST_CHECK_NE(s1.get(), s2.get());
}

BOOST_AUTO_TEST_CASE(TestAddSingletonAuto) {
    ServiceContainer container;
    container.add_singleton_auto<IMyService, MyServiceImpl>();

    auto s1 = container.get_service<IMyService>();
    auto s2 = container.get_service<IMyService>();

    BOOST_CHECK(s1 != nullptr);
    BOOST_CHECK_EQUAL(s1->greet(), "hello");
    // Singleton should return same instance
    BOOST_CHECK_EQUAL(s1.get(), s2.get());
}

BOOST_AUTO_TEST_CASE(TestAddTransientAutoSameType) {
    ServiceContainer container;
    container.add_transient_auto<SimpleService, SimpleService>();

    auto s = container.get_service<SimpleService>();
    BOOST_CHECK(s != nullptr);
    BOOST_CHECK_EQUAL(s->value(), 99);
}

BOOST_AUTO_TEST_CASE(TestAddSingletonAutoSameType) {
    ServiceContainer container;
    container.add_singleton_auto<SimpleService, SimpleService>();

    auto s1 = container.get_service<SimpleService>();
    auto s2 = container.get_service<SimpleService>();

    BOOST_CHECK(s1 != nullptr);
    BOOST_CHECK_EQUAL(s1.get(), s2.get());
}

BOOST_AUTO_TEST_CASE(TestHasDependenciesTrait) {
    // SimpleService doesn't have dependencies typedef
    BOOST_CHECK(!has_dependencies_v<SimpleService>);
}

BOOST_AUTO_TEST_CASE(TestCreateContainerHelper) {
    auto container = create_container();
    BOOST_CHECK(container != nullptr);
    BOOST_CHECK_EQUAL(container->service_count(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
