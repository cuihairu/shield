#pragma once

#include <string>

#include <boost/property_tree/ptree.hpp>
#include <yaml-cpp/yaml.h>

namespace shield::config {

YAML::Node ptree_to_yaml_node(const boost::property_tree::ptree& pt);
std::string ptree_to_yaml_string(const boost::property_tree::ptree& pt);

}  // namespace shield::config

