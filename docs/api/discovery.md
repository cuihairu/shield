# Discovery 服务发现 API 文档

服务发现模块提供分布式系统中服务注册、发现、健康检查和负载均衡功能。支持多种后端存储，包括 etcd、consul、nacos、redis 和本地内存。

## 📋 模块概览

服务发现模块包含以下主要类：

- `IServiceDiscovery`: 服务发现接口基类
- `ServiceInstance`: 服务实例描述
- `EtcdDiscovery`: etcd 后端实现
- `ConsulDiscovery`: consul 后端实现
- `NacosDiscovery`: nacos 后端实现
- `RedisDiscovery`: redis 后端实现
- `LocalDiscovery`: 本地内存实现

## 🔧 IServiceDiscovery 接口基类

定义了服务发现的统一接口，所有具体实现都必须遵循此接口。

### 接口定义

```cpp
namespace shield::discovery {

enum class ServiceStatus {
    UNKNOWN = 0,    // 未知状态
    HEALTHY = 1,    // 健康
    UNHEALTHY = 2,  // 不健康
    CRITICAL = 3    // 严重错误
};

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;
    
    // 服务注册
    virtual void register_service(const ServiceInstance& instance) = 0;
    virtual void unregister_service(const std::string& service_id) = 0;
    
    // 服务发现
    virtual std::vector<ServiceInstance> discover_services(const std::string& service_name) = 0;
    virtual std::optional<ServiceInstance> find_service(const std::string& service_id) = 0;
    
    // 健康检查
    virtual void start_heartbeat(const std::string& service_id) = 0;
    virtual void stop_heartbeat(const std::string& service_id) = 0;
    virtual void update_service_status(const std::string& service_id, ServiceStatus status) = 0;
    
    // 服务监听
    virtual void watch_service(const std::string& service_name, 
                              std::function<void(const std::vector<ServiceInstance>&)> callback) = 0;
    virtual void unwatch_service(const std::string& service_name) = 0;
    
    // 负载均衡
    virtual std::optional<ServiceInstance> select_service(const std::string& service_name,
                                                         const std::string& strategy = "random") = 0;
    
    // 生命周期
    virtual void initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool is_initialized() const = 0;
};

} // namespace shield::discovery
```

## 🏷️ ServiceInstance 服务实例

描述一个服务实例的完整信息。

### 类定义

```cpp
namespace shield::discovery {

struct ServiceInstance {
    std::string service_id;                                   // 服务实例 ID
    std::string service_name;                                // 服务名称
    std::string host;                                        // 主机地址
    uint16_t port;                                          // 端口号
    std::map<std::string, std::string> metadata;           // 元数据
    std::vector<std::string> tags;                          // 服务标签
    ServiceStatus status = ServiceStatus::HEALTHY;          // 服务状态
    std::chrono::system_clock::time_point register_time;    // 注册时间
    std::chrono::system_clock::time_point last_heartbeat;   // 最后心跳时间
    
    // 便捷方法
    std::string get_endpoint() const;                       // 获取 host:port
    void set_metadata(const std::string& key, const std::string& value);
    std::string get_metadata(const std::string& key, const std::string& default_value = "") const;
    bool has_tag(const std::string& tag) const;
    void add_tag(const std::string& tag);
    
    // 序列化支持
    nlohmann::json to_json() const;
    static ServiceInstance from_json(const nlohmann::json& json);
    
    // 比较操作
    bool operator==(const ServiceInstance& other) const;
    bool operator!=(const ServiceInstance& other) const;
};

} // namespace shield::discovery
```

### 使用示例

```cpp
// 创建服务实例
shield::discovery::ServiceInstance instance;
instance.service_id = "game-server-001";
instance.service_name = "game-server";
instance.host = "192.168.1.100";
instance.port = 8080;
instance.status = shield::discovery::ServiceStatus::HEALTHY;

// 设置元数据
instance.set_metadata("version", "1.0.0");
instance.set_metadata("region", "us-west");
instance.set_metadata("datacenter", "dc1");

// 添加标签
instance.add_tag("production");
instance.add_tag("game");
instance.add_tag("primary");

// 获取服务端点
std::string endpoint = instance.get_endpoint();  // "192.168.1.100:8080"

// JSON 序列化
auto json_data = instance.to_json();
auto restored_instance = shield::discovery::ServiceInstance::from_json(json_data);
```

## 🗃️ EtcdDiscovery etcd 后端

基于 etcd 的服务发现实现，适合大规模分布式部署。

### 类定义

```cpp
namespace shield::discovery {

struct EtcdConfig {
    std::vector<std::string> endpoints = {"http://127.0.0.1:2379"};  // etcd 集群地址
    std::string username;                                             // 用户名
    std::string password;                                             // 密码
    std::string ca_cert_path;                                        // CA 证书路径
    std::string cert_path;                                           // 客户端证书路径
    std::string key_path;                                            // 客户端私钥路径
    std::chrono::seconds connect_timeout{5};                        // 连接超时
    std::chrono::seconds request_timeout{10};                       // 请求超时
    std::chrono::seconds heartbeat_interval{30};                    // 心跳间隔
    std::chrono::seconds lease_ttl{60};                             // 租约 TTL
    std::string key_prefix = "/shield/services/";                   // 键前缀
};

class EtcdDiscovery : public IServiceDiscovery {
public:
    explicit EtcdDiscovery(const EtcdConfig& config);
    virtual ~EtcdDiscovery();
    
    // IServiceDiscovery 接口实现
    void register_service(const ServiceInstance& instance) override;
    void unregister_service(const std::string& service_id) override;
    std::vector<ServiceInstance> discover_services(const std::string& service_name) override;
    std::optional<ServiceInstance> find_service(const std::string& service_id) override;
    
    void start_heartbeat(const std::string& service_id) override;
    void stop_heartbeat(const std::string& service_id) override;
    void update_service_status(const std::string& service_id, ServiceStatus status) override;
    
    void watch_service(const std::string& service_name, 
                      std::function<void(const std::vector<ServiceInstance>&)> callback) override;
    void unwatch_service(const std::string& service_name) override;
    
    std::optional<ServiceInstance> select_service(const std::string& service_name,
                                                 const std::string& strategy) override;
    
    void initialize() override;
    void shutdown() override;
    bool is_initialized() const override;
    
    // etcd 特定方法
    std::vector<std::string> list_service_names();
    void cleanup_expired_services();
    bool is_service_healthy(const std::string& service_id);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::discovery
```

### 使用示例

```cpp
// 配置 etcd 连接
shield::discovery::EtcdConfig etcd_config;
etcd_config.endpoints = {"http://etcd1:2379", "http://etcd2:2379", "http://etcd3:2379"};
etcd_config.heartbeat_interval = std::chrono::seconds(15);
etcd_config.lease_ttl = std::chrono::seconds(45);

// 创建 etcd 服务发现
auto discovery = std::make_shared<shield::discovery::EtcdDiscovery>(etcd_config);
discovery->initialize();

// 注册服务
shield::discovery::ServiceInstance my_service;
my_service.service_id = "game-server-001";
my_service.service_name = "game-server";
my_service.host = "10.0.1.100";
my_service.port = 8080;
my_service.set_metadata("version", "1.2.0");
my_service.add_tag("production");

discovery->register_service(my_service);

// 启动心跳
discovery->start_heartbeat(my_service.service_id);

// 发现服务
auto services = discovery->discover_services("game-server");
for (const auto& service : services) {
    SHIELD_LOG_INFO << "发现服务: " << service.service_id 
                   << " 地址: " << service.get_endpoint();
}

// 负载均衡选择服务
auto selected_service = discovery->select_service("game-server", "round_robin");
if (selected_service) {
    SHIELD_LOG_INFO << "选择服务: " << selected_service->get_endpoint();
}

// 监听服务变化
discovery->watch_service("game-server", [](const std::vector<shield::discovery::ServiceInstance>& services) {
    SHIELD_LOG_INFO << "服务列表更新，当前服务数: " << services.size();
    for (const auto& service : services) {
        SHIELD_LOG_INFO << "  - " << service.service_id << ": " << service.get_endpoint();
    }
});
```

## 🏛️ ConsulDiscovery consul 后端

基于 Consul 的服务发现实现，提供丰富的服务治理功能。

### 类定义

```cpp
namespace shield::discovery {

struct ConsulConfig {
    std::string host = "127.0.0.1";                    // Consul 主机
    uint16_t port = 8500;                              // Consul 端口
    bool use_https = false;                            // 是否使用 HTTPS
    std::string token;                                 // 访问令牌
    std::string datacenter;                            // 数据中心
    std::chrono::seconds request_timeout{10};         // 请求超时
    std::chrono::seconds heartbeat_interval{30};      // 健康检查间隔
    std::chrono::seconds deregister_timeout{60};      // 注销超时
};

class ConsulDiscovery : public IServiceDiscovery {
public:
    explicit ConsulDiscovery(const ConsulConfig& config);
    virtual ~ConsulDiscovery();
    
    // IServiceDiscovery 接口实现
    void register_service(const ServiceInstance& instance) override;
    void unregister_service(const std::string& service_id) override;
    std::vector<ServiceInstance> discover_services(const std::string& service_name) override;
    std::optional<ServiceInstance> find_service(const std::string& service_id) override;
    
    void start_heartbeat(const std::string& service_id) override;
    void stop_heartbeat(const std::string& service_id) override;
    void update_service_status(const std::string& service_id, ServiceStatus status) override;
    
    void watch_service(const std::string& service_name, 
                      std::function<void(const std::vector<ServiceInstance>&)> callback) override;
    void unwatch_service(const std::string& service_name) override;
    
    std::optional<ServiceInstance> select_service(const std::string& service_name,
                                                 const std::string& strategy) override;
    
    void initialize() override;
    void shutdown() override;
    bool is_initialized() const override;
    
    // Consul 特定方法
    std::vector<std::string> list_datacenters();
    std::map<std::string, std::vector<ServiceInstance>> discover_all_services();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::discovery
```

## 🌐 NacosDiscovery nacos 后端

基于 Nacos 的服务发现实现，适合阿里云等环境。

### 类定义

```cpp
namespace shield::discovery {

struct NacosConfig {
    std::string server_addr = "127.0.0.1:8848";       // Nacos 服务器地址
    std::string namespace_id = "public";               // 命名空间
    std::string group_name = "DEFAULT_GROUP";          // 分组名称
    std::string username;                              // 用户名
    std::string password;                              // 密码
    std::chrono::seconds heartbeat_interval{5};       // 心跳间隔
    bool enable_load_balance = true;                   // 启用负载均衡
};

class NacosDiscovery : public IServiceDiscovery {
public:
    explicit NacosDiscovery(const NacosConfig& config);
    virtual ~NacosDiscovery();
    
    // IServiceDiscovery 接口实现 (省略重复定义)
    // ...

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::discovery
```

## 📦 RedisDiscovery redis 后端

基于 Redis 的轻量级服务发现实现。

### 类定义

```cpp
namespace shield::discovery {

struct RedisConfig {
    std::string host = "127.0.0.1";                   // Redis 主机
    uint16_t port = 6379;                             // Redis 端口
    std::string password;                              // 密码
    int database = 0;                                  // 数据库编号
    std::chrono::seconds connect_timeout{5};          // 连接超时
    std::chrono::seconds command_timeout{3};          // 命令超时
    std::chrono::seconds heartbeat_interval{30};      // 心跳间隔
    std::chrono::seconds service_ttl{90};             // 服务 TTL
    std::string key_prefix = "shield:services:";       // 键前缀
};

class RedisDiscovery : public IServiceDiscovery {
public:
    explicit RedisDiscovery(const RedisConfig& config);
    virtual ~RedisDiscovery();
    
    // IServiceDiscovery 接口实现 (省略重复定义)
    // ...

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::discovery
```

## 💾 LocalDiscovery 本地实现

基于内存的服务发现实现，适合开发和测试环境。

### 类定义

```cpp
namespace shield::discovery {

class LocalDiscovery : public IServiceDiscovery {
public:
    LocalDiscovery();
    virtual ~LocalDiscovery();
    
    // IServiceDiscovery 接口实现
    void register_service(const ServiceInstance& instance) override;
    void unregister_service(const std::string& service_id) override;
    std::vector<ServiceInstance> discover_services(const std::string& service_name) override;
    std::optional<ServiceInstance> find_service(const std::string& service_id) override;
    
    void start_heartbeat(const std::string& service_id) override;
    void stop_heartbeat(const std::string& service_id) override;
    void update_service_status(const std::string& service_id, ServiceStatus status) override;
    
    void watch_service(const std::string& service_name, 
                      std::function<void(const std::vector<ServiceInstance>&)> callback) override;
    void unwatch_service(const std::string& service_name) override;
    
    std::optional<ServiceInstance> select_service(const std::string& service_name,
                                                 const std::string& strategy) override;
    
    void initialize() override;
    void shutdown() override;
    bool is_initialized() const override;
    
    // 本地特定方法
    void clear_all_services();
    size_t get_service_count() const;
    std::vector<std::string> get_all_service_names() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::discovery
```

### 使用示例

```cpp
// 创建本地服务发现 (适合开发测试)
auto local_discovery = std::make_shared<shield::discovery::LocalDiscovery>();
local_discovery->initialize();

// 注册多个服务实例
for (int i = 1; i <= 3; ++i) {
    shield::discovery::ServiceInstance instance;
    instance.service_id = "game-server-" + std::to_string(i);
    instance.service_name = "game-server";
    instance.host = "192.168.1." + std::to_string(100 + i);
    instance.port = 8080;
    instance.set_metadata("zone", "zone-" + std::to_string(i));
    
    local_discovery->register_service(instance);
    local_discovery->start_heartbeat(instance.service_id);
}

// 发现服务
auto services = local_discovery->discover_services("game-server");
SHIELD_LOG_INFO << "发现 " << services.size() << " 个游戏服务器实例";

// 负载均衡测试
for (int i = 0; i < 10; ++i) {
    auto selected = local_discovery->select_service("game-server", "round_robin");
    if (selected) {
        SHIELD_LOG_INFO << "选择服务: " << selected->service_id;
    }
}
```

## 📚 最佳实践

### 1. 服务注册策略

```cpp
// ✅ 好的服务注册实践
class ServiceManager {
public:
    void register_with_retry(std::shared_ptr<IServiceDiscovery> discovery, 
                           const ServiceInstance& instance) {
        int retry_count = 0;
        const int max_retries = 3;
        
        while (retry_count < max_retries) {
            try {
                discovery->register_service(instance);
                discovery->start_heartbeat(instance.service_id);
                SHIELD_LOG_INFO << "服务注册成功: " << instance.service_id;
                return;
                
            } catch (const std::exception& e) {
                retry_count++;
                SHIELD_LOG_WARN << "服务注册失败 (尝试 " << retry_count << "/" << max_retries 
                               << "): " << e.what();
                
                if (retry_count < max_retries) {
                    std::this_thread::sleep_for(std::chrono::seconds(retry_count * 2));
                }
            }
        }
        
        throw std::runtime_error("服务注册最终失败");
    }
    
    void graceful_unregister(std::shared_ptr<IServiceDiscovery> discovery,
                           const std::string& service_id) {
        try {
            // 先停止心跳
            discovery->stop_heartbeat(service_id);
            
            // 等待一个心跳周期，让其他服务感知到状态变化
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // 注销服务
            discovery->unregister_service(service_id);
            SHIELD_LOG_INFO << "服务优雅下线完成: " << service_id;
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "服务下线失败: " << e.what();
        }
    }
};
```

### 2. 健康检查和故障处理

```cpp
// ✅ 健壮的健康检查
class HealthChecker {
public:
    void start_health_monitoring(std::shared_ptr<IServiceDiscovery> discovery,
                                const std::string& service_id) {
        m_health_thread = std::thread([this, discovery, service_id]() {
            while (m_running) {
                try {
                    auto health_status = check_service_health();
                    
                    shield::discovery::ServiceStatus status;
                    if (health_status.cpu_usage < 0.8 && health_status.memory_usage < 0.9) {
                        status = shield::discovery::ServiceStatus::HEALTHY;
                    } else if (health_status.cpu_usage < 0.95 && health_status.memory_usage < 0.95) {
                        status = shield::discovery::ServiceStatus::UNHEALTHY;
                    } else {
                        status = shield::discovery::ServiceStatus::CRITICAL;
                    }
                    
                    discovery->update_service_status(service_id, status);
                    
                } catch (const std::exception& e) {
                    SHIELD_LOG_ERROR << "健康检查失败: " << e.what();
                    discovery->update_service_status(service_id, 
                        shield::discovery::ServiceStatus::CRITICAL);
                }
                
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        });
    }

private:
    struct HealthStatus {
        double cpu_usage = 0.0;
        double memory_usage = 0.0;
        bool disk_healthy = true;
        bool network_healthy = true;
    };
    
    HealthStatus check_service_health() {
        // 实际的健康检查逻辑
        HealthStatus status;
        // ... 检查 CPU、内存、磁盘、网络等
        return status;
    }
    
    std::atomic<bool> m_running{true};
    std::thread m_health_thread;
};
```

### 3. 负载均衡策略

```cpp
// ✅ 智能负载均衡
class SmartLoadBalancer {
public:
    std::optional<ServiceInstance> select_best_service(
        const std::vector<ServiceInstance>& services) {
        
        if (services.empty()) {
            return std::nullopt;
        }
        
        // 过滤健康的服务
        std::vector<ServiceInstance> healthy_services;
        std::copy_if(services.begin(), services.end(), 
                    std::back_inserter(healthy_services),
                    [](const ServiceInstance& s) {
                        return s.status == ServiceStatus::HEALTHY;
                    });
        
        if (healthy_services.empty()) {
            // 如果没有健康服务，选择状态最好的
            auto best_service = std::min_element(services.begin(), services.end(),
                [](const ServiceInstance& a, const ServiceInstance& b) {
                    return static_cast<int>(a.status) < static_cast<int>(b.status);
                });
            return *best_service;
        }
        
        // 基于权重和响应时间选择
        return select_by_weighted_response_time(healthy_services);
    }

private:
    std::optional<ServiceInstance> select_by_weighted_response_time(
        const std::vector<ServiceInstance>& services) {
        
        // 获取每个服务的响应时间权重
        std::vector<double> weights;
        for (const auto& service : services) {
            double response_time = get_service_response_time(service.service_id);
            // 响应时间越短，权重越高
            weights.push_back(1.0 / (response_time + 0.01));
        }
        
        // 加权随机选择
        std::random_device rd;
        std::mt19937 gen(rd());
        std::discrete_distribution<> dist(weights.begin(), weights.end());
        
        int selected_index = dist(gen);
        return services[selected_index];
    }
    
    double get_service_response_time(const std::string& service_id) {
        // 从监控系统获取服务响应时间
        // 这里返回模拟数据
        return 0.1 + (std::rand() % 100) / 1000.0;  // 0.1-0.2秒
    }
};
```

---

服务发现模块是分布式系统的核心组件，提供了可靠的服务注册、发现和健康监控能力。通过支持多种后端存储，Shield 框架可以适应不同的部署环境和规模需求。