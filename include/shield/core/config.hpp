#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <sstream>
#include <yaml-cpp/yaml.h>

namespace shield::core {

class Config {
public:
    static Config& instance() {
        static Config instance;
        return instance;
    }

    void load(const std::string& file_path);
    void load_from_string(const std::string& yaml_content);
    void reset();

    template <typename T>
    T get(const std::string& key) const {
        try {
            YAML::Node node = YAML::Load(YAML::Dump(config_));
            std::stringstream ss(key);
            std::string segment;
            while(std::getline(ss, segment, '.')) {
                node = node[segment];
            }
            return node.as<T>();
        } catch (const YAML::Exception& e) {
            throw std::runtime_error("Config key not found: " + key + " (" + e.what() + ")");
        }
    }

private:
    Config() = default;
    YAML::Node config_;
};

} // namespace shield::core