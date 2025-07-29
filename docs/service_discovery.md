# 服务发现设计文档

本文档阐述了 `shield` 项目中服务发现模块的设计理念和实现策略。一个健壮、灵活的服务发现机制是构建分布式系统的基石。

## 1. 核心设计：接口与实现分离

为了使业务逻辑与具体的基础设施技术解耦，我们采用了**接口与实现分离**的设计模式。所有的服务发现功能都通过一个统一的抽象接口 `IServiceDiscovery` 来定义。

```cpp
// include/shield/discovery/service_discovery.hpp

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    // 注册服务
    virtual bool register_service(const ServiceInstance& instance) = 0;

    // 注销服务
    virtual bool deregister_service(const std::string& instance_id) = 0;

    // 查询单个服务实例（内置负载均衡）
    virtual std::optional<ServiceInstance> query_service(const std::string& service_name) = 0;

    // 查询所有服务实例
    virtual std::vector<ServiceInstance> query_all_services(const std::string& service_name) = 0;
};
```

这种设计带来了巨大的好处：

*   **可替换性**: 我们可以轻松地切换服务发现的后端技术（如从 `etcd` 切换到 `Consul`），而无需修改任何业务组件的代码。
*   **可测试性**: 我们可以创建一个用于测试的轻量级实现，让单元测试和集成测试变得简单、快速，且不依赖外部环境。

## 2. 实现方案

我们目前提供了两种 `IServiceDiscovery` 的实现，以应对不同的环境需求。

### 2.1. 生产环境: `EtcdServiceDiscovery`

*   **实现文件**: `src/discovery/etcd_discovery.cpp`
*   **后端技术**: `etcd`
*   **描述**: 这是为生产环境设计的、健壮的服务发现实现。它通过 `etcd` 的 C++ 客户端库与一个高可用的 `etcd` 集群通信。它利用 `etcd` 的租约（Lease）机制来实现服务的自动健康检查和故障转移。所有需要高可用和持久化的部署都应该使用此实现。

### 2.2. 开发与测试环境: `InMemoryServiceDiscovery`

*   **实现文件**: `src/discovery/in_memory_discovery.cpp`
*   **后端技术**: `std::map` 和 `std::mutex`
*   **描述**: 这是一个**为方便开发和测试而创建的轻量级内存注册中心**。它在程序内部的一个线程安全的 `map` 中维护服务列表，完全不依赖任何外部进程或网络连接。

#### 使用它的好处：

1.  **提升开发效率**: 开发者在本地编写和调试业务逻辑时，无需安装和配置一个完整的 `etcd` 集群。只需在配置中启用 `in-memory` 类型，即可立即运行整个系统。
2.  **简化自动化测试**: 在运行单元测试或集成测试时，我们可以即时创建一个 `InMemoryServiceDiscovery` 实例。测试代码可以完全控制服务的注册和注销，从而精确地模拟各种服务上线、下线的场景，而测试过程是毫秒级的，并且结果稳定、可重复。
3.  **零依赖**: 它使得项目的核心逻辑可以独立编译和运行，降低了新成员加入项目的门槛。

它的内部实现包含一个简单的随机负载均衡策略，用于模拟生产环境中的多实例场景。

## 3. 如何选择实现

在程序的入口（`main.cpp`），我们会根据配置文件 `config/shield.yaml` 来决定实例化哪个服务发现的实现。这通过一个工厂模式来完成。

**配置示例 (`config/shield.yaml`):**

```yaml
# 服务发现配置
discovery:
  # 可选值: "etcd", "in-memory"
  type: "in-memory" # 在开发时使用 in-memory

  # etcd 的专属配置，当 type 为 "etcd" 时生效
  etcd:
    endpoints: "http://127.0.0.1:2379"
```

**代码逻辑 (`main.cpp`):**

```cpp
// ...
std::shared_ptr<shield::discovery::IServiceDiscovery> discovery_service;

if (config.discovery.type == "etcd") {
    discovery_service = shield::discovery::make_etcd_discovery(config.discovery.etcd.endpoints);
} else if (config.discovery.type == "in-memory") {
    discovery_service = shield::discovery::make_in_memory_discovery();
} else {
    // 错误处理
}

// 将创建好的 discovery_service 注入到所有需要它的组件中
// ...
```

通过这种方式，我们实现了生产环境的健壮性和开发测试环境的便捷性的完美统一。
