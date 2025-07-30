# Modular Configuration System Usage Guide

## Overview

The new modular configuration system provides better type safety, module decoupling, and testing convenience. Each component now has its own configuration class that can be independently validated and managed.

## Core Concepts

### 1. Module Configuration Base Class (ModuleConfig)

All module configurations inherit from the `ModuleConfig` base class:

```cpp
class ModuleConfig {
public:
    virtual void from_yaml(const YAML::Node& node) = 0;  // Load configuration from YAML
    virtual YAML::Node to_yaml() const = 0;              // Convert to YAML
    virtual void validate() const {}                      // Validate configuration validity
    virtual std::string module_name() const = 0;         // Module name
};
```

### 2. Configuration Manager (ConfigManager)

Responsible for loading configuration files and managing all module configurations:

```cpp
// Load configuration file
ConfigManager::instance().load_config("config/app.yaml");

// Get specific module configuration
auto gateway_config = ConfigManager::instance().get_module_config<GatewayConfig>();
```

## How to Create New Module Configuration

### Step 1: Define Configuration Class

```cpp
// include/shield/your_module/your_module_config.hpp
#pragma once
#include "shield/config/module_config.hpp"

namespace shield::your_module {

class YourModuleConfig : public config::ModuleConfig {
public:
    // Configuration data structure
    struct ServerConfig {
        std::string host = "localhost";
        int port = 8080;
    };
    
    ServerConfig server;
    
    // Implement ModuleConfig interface
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "your_module"; }
};

} // namespace shield::your_module
```

### Step 2: Implement Configuration Class

```cpp
// src/your_module/your_module_config.cpp
#include "shield/your_module/your_module_config.hpp"

void YourModuleConfig::from_yaml(const YAML::Node& node) {
    if (node["server"]) {
        auto server_node = node["server"];
        if (server_node["host"]) server.host = server_node["host"].as<std::string>();
        if (server_node["port"]) server.port = server_node["port"].as<int>();
    }
}

YAML::Node YourModuleConfig::to_yaml() const {
    YAML::Node node;
    node["server"]["host"] = server.host;
    node["server"]["port"] = server.port;
    return node;
}

void YourModuleConfig::validate() const {
    if (server.host.empty()) {
        throw std::invalid_argument("Server host cannot be empty");
    }
    if (server.port <= 0 || server.port > 65535) {
        throw std::invalid_argument("Server port must be between 1 and 65535");
    }
}
```

### Step 3: Register Configuration

In `src/config/config_registry.cpp` add:

```cpp
#include "shield/your_module/your_module_config.hpp"

void register_all_module_configs() {
    // ... other configuration registrations
    ModuleConfigFactory<your_module::YourModuleConfig>::create_and_register();
}
```

### Step 4: Use in Components

```cpp
// include/shield/your_module/your_component.hpp
class YourComponent {
public:
    YourComponent(std::shared_ptr<YourModuleConfig> config);
    
private:
    std::shared_ptr<YourModuleConfig> m_config;
};

// src/your_module/your_component.cpp
YourComponent::YourComponent(std::shared_ptr<YourModuleConfig> config)
    : m_config(config) {
    
    // Use configuration
    std::string host = m_config->server.host;
    int port = m_config->server.port;
}
```

## Configuration File Structure

The new YAML configuration files support modular structure:

```yaml
# config/app.yaml
gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  tcp:
    enabled: true
    backlog: 128

prometheus:
  server:
    enabled: true
    port: 9090
    path: "/metrics"

your_module:
  server:
    host: "localhost"
    port: 8080
```

## Migration from Old Configuration System

### Old System (Not Recommended)
```cpp
void Component::init() {
    auto& config = shield::config::Config::instance();
    auto host = config.get<std::string>("gateway.listener.host");
    auto port = config.get<uint16_t>("gateway.listener.port");
}
```

### New System (Recommended)
```cpp
class Component {
public:
    Component(std::shared_ptr<GatewayConfig> config) : m_config(config) {}
    
    void init() {
        // Type-safe access
        const auto& host = m_config->listener.host;
        const auto& port = m_config->listener.port;
        
        // Automatic configuration validation
        m_config->validate();
    }
    
private:
    std::shared_ptr<GatewayConfig> m_config;
};
```

## Best Practices

### 1. Configuration Validation
Always check configuration validity in the `validate()` method:

```cpp
void YourConfig::validate() const {
    if (server.port <= 0) {
        throw std::invalid_argument("Port must be positive");
    }
    
    // Check for port conflicts
    if (http.enabled && http.port == server.port) {
        throw std::invalid_argument("HTTP port conflicts with server port");
    }
}
```

### 2. Default Values
Provide reasonable default values in configuration structures:

```cpp
struct ServerConfig {
    std::string host = "0.0.0.0";    // Default listen on all interfaces
    int port = 8080;                 // Default port
    bool enabled = true;             // Default enabled
};
```

### 3. Convenience Methods
Provide convenience methods to handle complex configuration logic:

```cpp
class GatewayConfig : public ModuleConfig {
public:
    // Convenience method: get effective thread count
    int get_effective_io_threads() const {
        return (threading.io_threads > 0) ? 
               threading.io_threads : 
               std::thread::hardware_concurrency();
    }
};
```

### 4. Dependency Injection
Accept configuration in component constructors rather than looking it up at runtime:

```cpp
// Good practice
class Component {
public:
    Component(std::shared_ptr<ComponentConfig> config);
};

// Avoid this
class Component {
public:
    Component() {
        // Looking up configuration in constructor - not recommended
        config_ = ConfigManager::instance().get_module_config<ComponentConfig>();
    }
};
```

## Testing Support

Modular configuration makes testing easier:

```cpp
TEST(ComponentTest, TestWithCustomConfig) {
    // Create test-specific configuration
    auto config = std::make_shared<GatewayConfig>();
    config->listener.host = "127.0.0.1";
    config->listener.port = 9999;
    config->validate();
    
    // Create component with test configuration
    GatewayComponent component("test", actor_system, lua_pool, config);
    
    // Test component behavior
    // ...
}
```

## Environment and Profile Support

Support for different environment configurations:

```cpp
// Load default configuration
ConfigManager::instance().load_config("config/app.yaml");

// Load configuration with profile
ConfigManager::instance().load_config_with_profile("production");
// This loads app.yaml + app-production.yaml
```

## Troubleshooting

### Common Errors

1. **Configuration not registered**
   ```
   Error: get_module_config returned null
   Solution: Make sure the configuration class is registered in config_registry.cpp
   ```

2. **YAML key mismatch**
   ```
   Error: Field has default value after configuration loading
   Solution: Check if YAML file key names match the key names in from_yaml()
   ```

3. **Configuration validation failed**
   ```
   Error: Configuration validation failed
   Solution: Check validation logic in validate() method and values in configuration file
   ```

Through this modular configuration system, your project will have better maintainability, type safety, and testing convenience.