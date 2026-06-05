#define BOOST_TEST_MODULE PluginHostTest
#include <boost/test/unit_test.hpp>

#include "shield/core/plugin_host.hpp"

using namespace shield::core;

BOOST_AUTO_TEST_SUITE(PluginHostTests)

BOOST_AUTO_TEST_CASE(TestDefaultConstruction) {
    PluginHost host;
    BOOST_CHECK(host.get_plugin_manager() == nullptr);
}

BOOST_AUTO_TEST_CASE(TestConfigureWithNullPluginManager) {
    PluginHost host;
    auto& ctx = ApplicationContext::instance();
    BOOST_CHECK_THROW(host.configure(ctx, std::unique_ptr<PluginManager>()),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestConfigureWithPluginManager) {
    PluginHost host;
    auto& ctx = ApplicationContext::instance();
    auto pm = std::make_unique<PluginManager>();
    BOOST_CHECK_NO_THROW(host.configure(ctx, std::move(pm)));
    BOOST_CHECK(host.get_plugin_manager() != nullptr);
}

BOOST_AUTO_TEST_CASE(TestConfigureWithPluginsDirectory) {
    PluginHost host;
    auto& ctx = ApplicationContext::instance();
    // Non-existent directory should still work (just discovers 0 plugins)
    BOOST_CHECK_NO_THROW(host.configure(ctx, "/nonexistent/plugins"));
    BOOST_CHECK(host.get_plugin_manager() != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
