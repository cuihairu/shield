#define BOOST_TEST_MODULE AdvancedContainerTest
#include <boost/test/unit_test.hpp>

#include <memory>

#include "shield/di/advanced_container.hpp"

using namespace shield::di;

// Test interfaces and implementations
struct ITestService {
    virtual ~ITestService() = default;
    virtual std::string name() const = 0;
};

class TestServiceA : public ITestService {
public:
    std::string name() const override { return "A"; }
};

class TestServiceB : public ITestService {
public:
    std::string name() const override { return "B"; }
};

class DefaultConstructible {
public:
    int value() const { return 42; }
};

BOOST_AUTO_TEST_SUITE(ResolutionContextTests)

BOOST_AUTO_TEST_CASE(TestPushAndIsResolving) {
    ResolutionContext ctx;
    auto type = std::type_index(typeid(ITestService));

    BOOST_CHECK(!ctx.is_resolving(type));
    ctx.push_resolution(type);
    BOOST_CHECK(ctx.is_resolving(type));
}

BOOST_AUTO_TEST_CASE(TestPopResolution) {
    ResolutionContext ctx;
    auto type = std::type_index(typeid(ITestService));

    ctx.push_resolution(type);
    BOOST_CHECK(ctx.is_resolving(type));
    ctx.pop_resolution(type);
    BOOST_CHECK(!ctx.is_resolving(type));
}

BOOST_AUTO_TEST_CASE(TestCircularDependencyDetection) {
    ResolutionContext ctx;
    auto type = std::type_index(typeid(ITestService));

    ctx.push_resolution(type);
    BOOST_CHECK_THROW(ctx.push_resolution(type), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TestGetResolutionChain) {
    ResolutionContext ctx;
    auto type1 = std::type_index(typeid(ITestService));
    auto type2 = std::type_index(typeid(DefaultConstructible));

    ctx.push_resolution(type1);
    ctx.push_resolution(type2);

    auto chain = ctx.get_resolution_chain();
    BOOST_CHECK_EQUAL(chain.size(), 2u);
}

BOOST_AUTO_TEST_CASE(TestEmptyChain) {
    ResolutionContext ctx;
    auto chain = ctx.get_resolution_chain();
    BOOST_CHECK(chain.empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ResolutionGuardTests)

BOOST_AUTO_TEST_CASE(TestGuardRAII) {
    ResolutionContext ctx;
    auto type = std::type_index(typeid(ITestService));

    {
        ResolutionGuard guard(ctx, type);
        BOOST_CHECK(ctx.is_resolving(type));
    }
    // After guard is destroyed, type should no longer be resolving
    BOOST_CHECK(!ctx.is_resolving(type));
}

BOOST_AUTO_TEST_CASE(TestGuardNested) {
    ResolutionContext ctx;
    auto type1 = std::type_index(typeid(ITestService));
    auto type2 = std::type_index(typeid(DefaultConstructible));

    {
        ResolutionGuard guard1(ctx, type1);
        BOOST_CHECK(ctx.is_resolving(type1));

        {
            ResolutionGuard guard2(ctx, type2);
            BOOST_CHECK(ctx.is_resolving(type1));
            BOOST_CHECK(ctx.is_resolving(type2));
        }

        BOOST_CHECK(ctx.is_resolving(type1));
        BOOST_CHECK(!ctx.is_resolving(type2));
    }

    BOOST_CHECK(!ctx.is_resolving(type1));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(AdvancedContainerTests)

BOOST_AUTO_TEST_CASE(TestDefaultConstruction) {
    AdvancedContainer container;
    BOOST_CHECK(!container.can_resolve<ITestService>());
}

BOOST_AUTO_TEST_CASE(TestRegisterAutoInject) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();

    BOOST_CHECK(container.can_resolve<ITestService>());
}

BOOST_AUTO_TEST_CASE(TestResolveAutoInject) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();

    auto service = container.resolve<ITestService>();
    BOOST_CHECK(service != nullptr);
    BOOST_CHECK_EQUAL(service->name(), "A");
}

BOOST_AUTO_TEST_CASE(TestResolveSingleton) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();

    auto s1 = container.resolve<ITestService>();
    auto s2 = container.resolve<ITestService>();
    BOOST_CHECK_EQUAL(s1.get(), s2.get());
}

BOOST_AUTO_TEST_CASE(TestTryResolveRegistered) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();

    auto service = container.try_resolve<ITestService>();
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(TestTryResolveUnregistered) {
    AdvancedContainer container;

    auto service = container.try_resolve<ITestService>();
    BOOST_CHECK(service == nullptr);
}

BOOST_AUTO_TEST_CASE(TestCanResolve) {
    AdvancedContainer container;
    BOOST_CHECK(!container.can_resolve<ITestService>());

    container.register_auto_inject<ITestService, TestServiceA>();
    BOOST_CHECK(container.can_resolve<ITestService>());
}

BOOST_AUTO_TEST_CASE(TestRegisterFactoryAdvanced) {
    AdvancedContainer container;
    container.register_factory_advanced<ITestService>(
        [](AdvancedContainer&) { return std::make_shared<TestServiceB>(); });

    auto service = container.resolve<ITestService>();
    BOOST_CHECK(service != nullptr);
    BOOST_CHECK_EQUAL(service->name(), "B");
}

BOOST_AUTO_TEST_CASE(TestDefaultConstructible) {
    AdvancedContainer container;
    container.register_auto_inject<DefaultConstructible>();

    auto obj = container.resolve<DefaultConstructible>();
    BOOST_CHECK(obj != nullptr);
    BOOST_CHECK_EQUAL(obj->value(), 42);
}

BOOST_AUTO_TEST_CASE(TestOverrideRegistration) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();

    auto s1 = container.resolve<ITestService>();
    BOOST_CHECK_EQUAL(s1->name(), "A");

    container.register_auto_inject<ITestService, TestServiceB>();
    auto s2 = container.resolve<ITestService>();
    BOOST_CHECK_EQUAL(s2->name(), "B");
}

BOOST_AUTO_TEST_CASE(TestMultipleTypes) {
    AdvancedContainer container;
    container.register_auto_inject<ITestService, TestServiceA>();
    container.register_auto_inject<DefaultConstructible>();

    auto service = container.resolve<ITestService>();
    auto obj = container.resolve<DefaultConstructible>();

    BOOST_CHECK(service != nullptr);
    BOOST_CHECK(obj != nullptr);
    BOOST_CHECK_EQUAL(service->name(), "A");
    BOOST_CHECK_EQUAL(obj->value(), 42);
}

BOOST_AUTO_TEST_SUITE_END()
