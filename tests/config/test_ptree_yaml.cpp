// tests/config/test_ptree_yaml.cpp
#define BOOST_TEST_MODULE PtreeYamlTests
#include <boost/test/unit_test.hpp>

#include <string>

#include <boost/property_tree/ptree.hpp>
#include <yaml-cpp/yaml.h>

#include "shield/config/ptree_yaml.hpp"

namespace pt = boost::property_tree;

BOOST_AUTO_TEST_SUITE(PtreeYaml)

BOOST_AUTO_TEST_CASE(map_and_scalar_roundtrip) {
    pt::ptree tree;
    tree.put("server.host", "localhost");
    tree.put("server.port", "8080");

    const auto yaml = shield::config::ptree_to_yaml_string(tree);
    const auto node = YAML::Load(yaml);

    BOOST_CHECK_EQUAL(node["server"]["host"].as<std::string>(), "localhost");
    BOOST_CHECK_EQUAL(node["server"]["port"].as<std::string>(), "8080");
}

BOOST_AUTO_TEST_CASE(sequence_emits_as_yaml_sequence) {
    pt::ptree list;
    pt::ptree item1;
    item1.put("", "a");
    pt::ptree item2;
    item2.put("", "b");

    list.push_back(std::make_pair("", item1));
    list.push_back(std::make_pair("", item2));

    pt::ptree root;
    root.add_child("items", list);

    const auto yaml = shield::config::ptree_to_yaml_string(root);
    const auto node = YAML::Load(yaml);

    BOOST_REQUIRE(node["items"].IsSequence());
    BOOST_REQUIRE_EQUAL(node["items"].size(), 2U);
    BOOST_CHECK_EQUAL(node["items"][0].as<std::string>(), "a");
    BOOST_CHECK_EQUAL(node["items"][1].as<std::string>(), "b");
}

BOOST_AUTO_TEST_SUITE_END()

