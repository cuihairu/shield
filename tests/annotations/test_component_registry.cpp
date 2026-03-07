#define BOOST_TEST_MODULE ComponentRegistryTests
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "shield/annotations/component_registry.hpp"
#include "shield/core/application_context.hpp"
#include "shield/core/service.hpp"
#include "shield/di/advanced_container.hpp"

namespace shield::annotations {

class TestAnnotatedComponent {
public:
    int value() const { return 11; }
};

class TestAnnotatedService : public core::Service {
public:
    std::string name() const override { return "TestAnnotatedService"; }
    int value() const { return 22; }
};

class TestAnnotatedConfiguration {
public:
    std::string environment() const { return "test"; }
};

BOOST_AUTO_TEST_SUITE(ComponentRegistryTests)

BOOST_AUTO_TEST_CASE(test_auto_configure_application_context) {
    static std::atomic<int> counter{0};
    const int suffix = counter.fetch_add(1);
    const std::string component_name =
        "annotated_component_" + std::to_string(suffix);
    const std::string service_name =
        "annotated_service_" + std::to_string(suffix);
    const std::string config_name =
        "annotated_config_" + std::to_string(suffix);

    ComponentRegistry::register_component<TestAnnotatedComponent>(
        ComponentMetadata(component_name, component_name));
    ComponentRegistry::register_service<TestAnnotatedService>(
        ServiceMetadata(service_name, service_name));
    ComponentRegistry::register_configuration<TestAnnotatedConfiguration>(
        ConfigurationMetadata(config_name));

    auto& context = core::ApplicationContext::instance();
    ComponentRegistry::instance().auto_configure(context);

    auto component = context.get_bean<TestAnnotatedComponent>(component_name);
    BOOST_REQUIRE(component != nullptr);
    BOOST_CHECK_EQUAL(component->value(), 11);

    auto service = context.get_service<TestAnnotatedService>();
    BOOST_REQUIRE(service != nullptr);
    BOOST_CHECK_EQUAL(service->value(), 22);

    auto config = context.get_bean<TestAnnotatedConfiguration>(config_name);
    BOOST_REQUIRE(config != nullptr);
    BOOST_CHECK_EQUAL(config->environment(), "test");
}

BOOST_AUTO_TEST_CASE(test_auto_configure_advanced_container) {
    ComponentRegistry::register_component<TestAnnotatedComponent>(
        ComponentMetadata("test_comp_for_di", "test_comp_for_di"));

    di::AdvancedContainer container;
    ComponentRegistry::instance().auto_configure(container);

    auto component = container.resolve<TestAnnotatedComponent>();
    BOOST_REQUIRE(component != nullptr);
    BOOST_CHECK_EQUAL(component->value(), 11);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::annotations
