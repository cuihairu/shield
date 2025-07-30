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
    fs::path create_temp_yaml(const std::string &filename,
                              const std::string &content) {
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

    shield::config::Config &config = shield::config::Config::instance();
    BOOST_CHECK_NO_THROW(config.load(config_path.string()));

    BOOST_CHECK_EQUAL(config.get<std::string>("server.host"), "localhost");
    BOOST_CHECK_EQUAL(config.get<int>("server.port"), 8080);
}

BOOST_AUTO_TEST_CASE(test_invalid_file_path) {
    shield::config::Config &config = shield::config::Config::instance();
    // Assume load method throws exception or returns false when file doesn't
    // exist
    BOOST_CHECK_THROW(
        config.load("non_existent_file.yaml"),
        std::runtime_error);  // or other exception type you defined
}

BOOST_AUTO_TEST_SUITE_END()
