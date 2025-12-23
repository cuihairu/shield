# 服务发现

Shield 支持多种服务发现后端：Nacos、Consul、Etcd、Redis。

## ServiceInstance

```cpp
#include <shield/discovery/service_instance.hpp>

shield::discovery::ServiceInstance instance;
instance.instance_id = "gateway-001";
instance.service_name = "gateway";
instance.address = "192.168.1.100";
instance.port = 8080;
```

## Nacos

```cpp
#include <shield/discovery/nacos_discovery.hpp>

shield::discovery::NacosDiscovery discovery;
discovery.initialize("http://localhost:8848");

// 注册服务
discovery.register_service(instance);

// 发现服务
auto instances = discovery.get_instances("gateway");
```

## Consul

```cpp
#include <shield/discovery/consul_discovery.hpp>

shield::discovery::ConsulDiscovery discovery;
discovery.initialize("http://localhost:8500");
discovery.register_service(instance);
```

## Etcd

```cpp
#include <shield/discovery/etcd_discovery.hpp>

shield::discovery::EtcdDiscovery discovery;
discovery.initialize("http://localhost:2379");
discovery.register_service(instance);
```

## Redis

```cpp
#include <shield/discovery/redis_discovery.hpp>

shield::discovery::RedisDiscovery discovery;
discovery.initialize("redis://localhost:6379");
discovery.register_service(instance);
```

## LocalDiscovery

本地内存实现，用于开发测试：

```cpp
#include <shield/discovery/local_discovery.hpp>

shield::discovery::LocalDiscovery discovery;
discovery.register_service(instance);
auto instances = discovery.get_instances("gateway");
```

## DiscoveryConfig

```cpp
#include <shield/discovery/discovery_config.hpp>

shield::discovery::DiscoveryConfig config;
config.backend = "nacos";  // consul, etcd, redis, local
config.server_addr = "http://localhost:8848";
config.namespace_id = "public";
```
