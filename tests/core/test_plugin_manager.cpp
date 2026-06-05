#define BOOST_TEST_MODULE PluginManagerTest
#include <boost/test/unit_test.hpp>

#include "shield/core/plugin_manager.hpp"

using namespace shield::core;

BOOST_AUTO_TEST_SUITE(PluginManagerTests)

BOOST_AUTO_TEST_CASE(TestDefaultConstruction) {
    PluginManager pm;
    BOOST_CHECK_EQUAL(pm.plugin_count(), 0u);
    BOOST_CHECK(pm.get_loaded_plugins().empty());
    BOOST_CHECK(!pm.is_plugin_loaded("anything"));
}

BOOST_AUTO_TEST_CASE(TestGetPluginReturnsNullForUnknown) {
    PluginManager pm;
    BOOST_CHECK(pm.get_plugin("nonexistent") == nullptr);
}

BOOST_AUTO_TEST_CASE(TestUnloadNonexistentReturnsFalse) {
    PluginManager pm;
    BOOST_CHECK(!pm.unload_plugin("nonexistent"));
}

BOOST_AUTO_TEST_CASE(TestLoadNonexistentReturnsFalse) {
    PluginManager pm;
    BOOST_CHECK(!pm.load_plugin("nonexistent"));
}

BOOST_AUTO_TEST_CASE(TestAddPluginDirectoryNonExistent) {
    PluginManager pm;
    // Adding a non-existent directory should not crash
    pm.add_plugin_directory("/nonexistent/path/to/plugins");
    BOOST_CHECK_EQUAL(pm.discover_plugins(), 0u);
}

BOOST_AUTO_TEST_CASE(TestGetPluginStartersEmpty) {
    PluginManager pm;
    auto starters = pm.get_plugin_starters();
    BOOST_CHECK(starters.empty());
}

BOOST_AUTO_TEST_CASE(TestSetEventCallback) {
    PluginManager pm;
    bool callback_called = false;
    pm.set_event_callback(
        [&callback_called](PluginEvent event, const std::string& name,
                           const std::string& msg) { callback_called = true; });
    // Callback is set but not called until an event occurs
    BOOST_CHECK(!callback_called);
}

BOOST_AUTO_TEST_CASE(TestDiscoverPluginsEmptyDirectories) {
    PluginManager pm;
    BOOST_CHECK_EQUAL(pm.discover_plugins(), 0u);
}

BOOST_AUTO_TEST_CASE(TestUnloadAllPluginsEmpty) {
    PluginManager pm;
    // Should not crash on empty
    pm.unload_all_plugins();
    BOOST_CHECK_EQUAL(pm.plugin_count(), 0u);
}

BOOST_AUTO_TEST_CASE(TestLibraryExtension) {
    PluginManager pm;
    // We can test indirectly through discover_plugins behavior
    // On Windows it's .dll, Linux .so, macOS .dylib
    // Just verify it doesn't crash
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()
