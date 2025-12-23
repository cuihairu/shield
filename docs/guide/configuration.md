# 配置管理

Shield 使用 YAML 配置文件，支持热更新。

## 配置结构

```yaml
# 日志配置
log:
  global_level: info
  file: logs/shield.log
  max_size: 100M
  max_files: 10

# Actor 配置
actor:
  worker_threads: 4
  middleman_network_backend: asio

# 网络配置
network:
  master_reactor_threads: 1
  slave_reactor_threads: 4

# 脚本配置
script:
  lua_vm_pool:
    min_size: 2
    max_size: 10
    script_path: scripts/

# 服务发现
discovery:
  backend: local
  server_addr: ""

# 网关
gateways:
  - name: tcp_gateway
    port: 8080
    protocol: tcp
  - name: ws_gateway
    port: 8081
    protocol: websocket
```

## Config 基类

```cpp
#include <shield/config/config.hpp>

class MyConfig : public shield::config::Config {
public:
    void from_yaml(const YAML::Node& node) override {
        // 解析 YAML
    }

    void to_yaml(YAML::Node& node) const override {
        // 生成 YAML
    }
};
```

## ConfigRegistry

```cpp
#include <shield/config/config_registry.hpp>

auto& registry = shield::config::ConfigRegistry::instance();

// 注册配置
registry.register_config("my_config", std::make_shared<MyConfig>());

// 获取配置
auto config = registry.get_config<MyConfig>("my_config");
```

## DynamicConfig

```cpp
#include <shield/config/dynamic_config.hpp>

shield::config::DynamicConfig dynamic_config;
dynamic_config.watch("config/shield.yaml", [](const std::string& path) {
    // 配置变更回调
});
```

## 热更新

```cpp
#include <shield/fs/file_watcher.hpp>

auto watcher = shield::fs::FileWatcher::create();
watcher->watch("config/shield.yaml", [](const std::string& path) {
    SHIELD_LOG_INFO << "Config changed: " << path;
    // 重新加载配置
});
```
