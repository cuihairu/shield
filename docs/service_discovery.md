# 服务发现

Shield 提供统一的服务发现抽象接口 `IServiceDiscovery`，支持多种后端实现。

## 接口定义

```cpp
// include/shield/discovery/service_discovery.hpp

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    virtual bool register_service(const ServiceInstance& instance) = 0;
    virtual bool deregister_service(const std::string& instance_id) = 0;
    virtual std::optional<ServiceInstance> query_service(const std::string& service_name) = 0;
    virtual std::vector<ServiceInstance> query_all_services(const std::string& service_name) = 0;
};
```

## 后端实现

| 后端 | 用途 | 说明 |
|------|------|------|
| **Static** | 开发 | 配置文件静态节点列表，零外部依赖 |
| **Redis** | 轻量生产 | 基于 Redis 的注册/发现 |
| **Nacos** | 生产 | 阿里云 Nacos 服务发现 |
| **Consul** | 生产 | HashiCorp Consul 服务发现 |
| **Etcd** | 生产 | CoreOS etcd 服务发现 |

## 配置

### 开发环境：Static

```yaml
discovery:
  type: "static"
  static:
    nodes:
      - "127.0.0.1:8080"
```

无需安装任何外部服务，适合本地开发和测试。

### 生产环境：Etcd

```yaml
discovery:
  type: "etcd"
  etcd:
    endpoints:
      - "http://etcd1:2379"
      - "http://etcd2:2379"
      - "http://etcd3:2379"
```

### 生产环境：Consul

```yaml
discovery:
  type: "consul"
  consul:
    host: "consul.cluster"
    port: 8500
```

### 生产环境：Nacos

```yaml
discovery:
  type: "nacos"
  nacos:
    server_addr: "nacos:8848"
    namespace: "public"
```

### 生产环境：Redis

```yaml
discovery:
  type: "redis"
  redis:
    host: "redis.cluster"
    port: 6379
```

## 设计原则

- **接口与实现分离**：业务逻辑不依赖具体后端，切换后端只需改配置
- **内置负载均衡**：`query_service()` 内部随机选择一个可用实例
- **零外部依赖启动**：使用 `static` 后端时无需安装任何外部服务
