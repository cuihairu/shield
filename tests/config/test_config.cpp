// tests/config/test_config.cpp
#define BOOST_TEST_MODULE ConfigTests
#include <boost/filesystem.hpp>  // for file operations
#include <boost/test/unit_test.hpp>
#include <fstream>

#include "shield/config/config.hpp"  // updated path

namespace fs = boost::filesystem;

// Test fixture: create and cleanup temporary YAML files
struct ConfigFixture {
    const fs::path temp_dir = fs::temp_directory_path() / "shield_test_configs";

    ConfigFixture() { fs::create_directories(temp_dir); }

    ~ConfigFixture() {
        fs::remove_all(temp_dir);  // cleanup temporary directory
    }

    // Helper function: create temporary YAML file
    fs::path create_temp_yaml(const std::string& filename,
                              const std::string& content) {
        fs::path file_path = temp_dir / filename;
        std::ofstream ofs(file_path.string());
        ofs << content;
        ofs.close();
        return file_path;
    }
};

// Apply fixture to all test cases
BOOST_GLOBAL_FIXTURE(ConfigFixture);

BOOST_AUTO_TEST_SUITE(ConfigTestSuite)

BOOST_AUTO_TEST_CASE(test_load_nested_config) {
    const std::string yaml_content = R"(
server:
  host: localhost
  port: 8080
database:
  type: sqlite
  path: /var/data/db.sqlite
)";

    // Create the fixture instance to access helper functions
    ConfigFixture fixture;
    fs::path config_path =
        fixture.create_temp_yaml("nested.yaml", yaml_content);

    auto& config_manager = shield::config::ConfigManager::instance();

    // Reset config manager to ensure clean state
    config_manager.reset();

    BOOST_CHECK_NO_THROW(config_manager.load_config(
        config_path.string(), shield::config::ConfigFormat::YAML));

    // Access configuration through the property tree
    const auto& config_tree = config_manager.get_config_tree();
    BOOST_CHECK_EQUAL(config_tree.get<std::string>("server.host"), "localhost");
    BOOST_CHECK_EQUAL(config_tree.get<int>("server.port"), 8080);
    BOOST_CHECK_EQUAL(config_tree.get<std::string>("database.type"), "sqlite");
    BOOST_CHECK_EQUAL(config_tree.get<std::string>("database.path"),
                      "/var/data/db.sqlite");
}

BOOST_AUTO_TEST_CASE(test_invalid_file_path) {
    auto& config_manager = shield::config::ConfigManager::instance();

    // Reset config manager to ensure clean state
    config_manager.reset();

    // Test that loading a non-existent file throws an exception
    BOOST_CHECK_THROW(
        config_manager.load_config("non_existent_file.yaml",
                                   shield::config::ConfigFormat::YAML),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_config_manager_singleton) {
    // Test that ConfigManager is indeed a singleton
    auto& config1 = shield::config::ConfigManager::instance();
    auto& config2 = shield::config::ConfigManager::instance();

    BOOST_CHECK_EQUAL(&config1, &config2);
}

BOOST_AUTO_TEST_CASE(test_config_formats) {
    ConfigFixture fixture;

    // Test YAML format
    const std::string yaml_content = R"(
test:
  value: yaml_test
)";

    fs::path yaml_path = fixture.create_temp_yaml("test.yaml", yaml_content);
    auto& config_manager = shield::config::ConfigManager::instance();
    config_manager.reset();

    BOOST_CHECK_NO_THROW(config_manager.load_config(
        yaml_path.string(), shield::config::ConfigFormat::YAML));

    const auto& config_tree = config_manager.get_config_tree();
    BOOST_CHECK_EQUAL(config_tree.get<std::string>("test.value"), "yaml_test");
}

BOOST_AUTO_TEST_CASE(test_config_reset) {
    ConfigFixture fixture;

    const std::string yaml_content = R"(
temp:
  data: should_be_reset
)";

    fs::path config_path =
        fixture.create_temp_yaml("reset_test.yaml", yaml_content);
    auto& config_manager = shield::config::ConfigManager::instance();

    // Load config
    config_manager.load_config(config_path.string(),
                               shield::config::ConfigFormat::YAML);

    // Verify data is loaded
    const auto& config_tree = config_manager.get_config_tree();
    BOOST_CHECK_EQUAL(config_tree.get<std::string>("temp.data"),
                      "should_be_reset");

    // Reset and verify config is empty
    config_manager.reset();
    const auto& empty_tree = config_manager.get_config_tree();
    BOOST_CHECK_THROW(empty_tree.get<std::string>("temp.data"),
                      boost::property_tree::ptree_bad_path);
}

BOOST_AUTO_TEST_SUITE_END()