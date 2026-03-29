#include "shield/config/ptree_yaml.hpp"

#include <stdexcept>

namespace shield::config {

namespace {

bool is_sequence_ptree(const boost::property_tree::ptree& pt) {
    if (pt.empty()) {
        return false;
    }
    for (const auto& child : pt) {
        if (!child.first.empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace

YAML::Node ptree_to_yaml_node(const boost::property_tree::ptree& pt) {
    if (pt.empty()) {
        if (!pt.data().empty()) {
            return YAML::Node(pt.data());
        }
        return YAML::Node(YAML::NodeType::Null);
    }

    if (is_sequence_ptree(pt)) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const auto& child : pt) {
            node.push_back(ptree_to_yaml_node(child.second));
        }
        return node;
    }

    YAML::Node node(YAML::NodeType::Map);
    for (const auto& child : pt) {
        node[child.first] = ptree_to_yaml_node(child.second);
    }
    return node;
}

std::string ptree_to_yaml_string(const boost::property_tree::ptree& pt) {
    YAML::Emitter out;
    out.SetIndent(2);
    out << ptree_to_yaml_node(pt);
    if (!out.good()) {
        throw std::runtime_error(std::string("YAML emit failed: ") +
                                 out.GetLastError());
    }
    return std::string(out.c_str());
}

}  // namespace shield::config

