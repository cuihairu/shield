// [CORE]
#pragma once

#include <string>

#include <yaml-cpp/yaml.h>

#include <boost/property_tree/ptree.hpp>

namespace shield::config {

YAML::Node ptree_to_yaml_node(const boost::property_tree::ptree& pt);
std::string ptree_to_yaml_string(const boost::property_tree::ptree& pt);

}  // namespace shield::config

