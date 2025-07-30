# 模块化配置系统使用指南

## 概述

新的模块化配置系统提供了更好的类型安全性、模块解耦和测试便利性。每个组件现在有自己的配置类，可以独立验证和管理。

## 核心概念

### 1. 模块配置基类 (ModuleConfig)

所有模块配置都继承自 `ModuleConfig` 基类：

```cpp
class ModuleConfig {
public:
    virtual void from_yaml(const YAML::Node& node) = 0;  // 从YAML加载配置
    virtual YAML::Node to_yaml() const = 0;              // 转换为YAML
    virtual void validate() const {}                      // 验证配置有效性
    virtual std::string module_name() const = 0;         // 模块名称
};
```

### 2. 配置管理器 (ConfigManager)

负责加载配置文件并管理所有模块配置：

```cpp
// 加载配置文件
ConfigManager::instance().load_config("config/app.yaml");

// 获取特定模块配置
auto gateway_config = ConfigManager::instance().get_module_config<GatewayConfig>();
```

## 如何创建新的模块配置

### 步骤1：定义配置类

```cpp
// include/shield/your_module/your_module_config.hpp
#pragma once
#include "shield/config/module_config.hpp"

namespace shield::your_module {

class YourModuleConfig : public config::ModuleConfig {
public:
    // 配置数据结构
    struct ServerConfig {
        std::string host = "localhost";
        int port = 8080;
    };
    
    ServerConfig server;
    
    // 实现ModuleConfig接口
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "your_module"; }
};

} // namespace shield::your_module
```

### 步骤2：实现配置类

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

### 步骤3：注册配置

在 `src/config/config_registry.cpp` 中添加：

```cpp
#include "shield/your_module/your_module_config.hpp"

void register_all_module_configs() {
    // ... 其他配置注册
    ModuleConfigFactory<your_module::YourModuleConfig>::create_and_register();
}
```

### 步骤4：在组件中使用

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
    
    // 使用配置
    std::string host = m_config->server.host;
    int port = m_config->server.port;
}
```

## 配置文件结构

新的YAML配置文件支持模块化结构：

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

## 从旧配置系统迁移

### 旧系统 (不推荐)
```cpp
void Component::init() {
    auto& config = shield::config::Config::instance();
    auto host = config.get<std::string>("gateway.listener.host");
    auto port = config.get<uint16_t>("gateway.listener.port");
}
```

### 新系统 (推荐)
```cpp
class Component {
public:
    Component(std::shared_ptr<GatewayConfig> config) : m_config(config) {}
    
    void init() {
        // 类型安全访问
        const auto& host = m_config->listener.host;
        const auto& port = m_config->listener.port;
        
        // 配置自动验证
        m_config->validate();
    }
    
private:
    std::shared_ptr<GatewayConfig> m_config;
};
```

## 最佳实践

### 1. 配置验证
始终在 `validate()` 方法中检查配置的有效性：

```cpp
void YourConfig::validate() const {
    if (server.port <= 0) {
        throw std::invalid_argument("Port must be positive");
    }
    
    // 检查端口冲突
    if (http.enabled && http.port == server.port) {
        throw std::invalid_argument("HTTP port conflicts with server port");
    }
}
```

### 2. 默认值
在配置结构中提供合理的默认值：

```cpp
struct ServerConfig {
    std::string host = "0.0.0.0";    // 默认监听所有接口
    int port = 8080;                 // 默认端口
    bool enabled = true;             // 默认启用
};
```

### 3. 便利方法
提供便利方法来处理复杂的配置逻辑：

```cpp
class GatewayConfig : public ModuleConfig {
public:
    // 便利方法：获取有效的线程数
    int get_effective_io_threads() const {
        return (threading.io_threads > 0) ? 
               threading.io_threads : 
               std::thread::hardware_concurrency();
    }
};
```

### 4. 依赖注入
在组件构造函数中接收配置，而不是在运行时查找：

```cpp
// 好的做法
class Component {
public:
    Component(std::shared_ptr<ComponentConfig> config);
};

// 避免这样做
class Component {
public:
    Component() {
        // 在构造函数中查找配置 - 不推荐
        config_ = ConfigManager::instance().get_module_config<ComponentConfig>();
    }
};
```

## 测试支持

模块化配置使测试变得更容易：

```cpp
TEST(ComponentTest, TestWithCustomConfig) {
    // 创建测试专用配置
    auto config = std::make_shared<GatewayConfig>();
    config->listener.host = "127.0.0.1";
    config->listener.port = 9999;
    config->validate();
    
    // 使用测试配置创建组件
    GatewayComponent component("test", actor_system, lua_pool, config);
    
    // 测试组件行为
    // ...
}
```

## 环境和Profile支持

支持不同环境的配置：

```cpp
// 加载默认配置
ConfigManager::instance().load_config("config/app.yaml");

// 加载带profile的配置
ConfigManager::instance().load_config_with_profile("production");
// 这会加载 app.yaml + app-production.yaml
```

## 故障排除

### 常见错误

1. **配置未注册**
   ```
   错误: get_module_config returned null
   解决: 确保在 config_registry.cpp 中注册了配置类
   ```

2. **YAML键不匹配**
   ```
   错误: 配置加载后字段为默认值
   解决: 检查YAML文件中的键名是否与 from_yaml() 中的键名一致
   ```

3. **配置验证失败**
   ```
   错误: Configuration validation failed
   解决: 检查 validate() 方法中的验证逻辑和配置文件中的值
   ```

通过这个模块化配置系统，你的项目将具有更好的可维护性、类型安全性和测试便利性。