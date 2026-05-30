# API 参考

Shield C++ / Lua API 参考文档。

## 命名空间

所有公共 C++ API 位于 `shield` 命名空间下。

```cpp
#include <shield/version.hpp>

std::cout << shield::get_version_string() << std::endl;
```

## 模块索引

- [核心 API](./core.md) — ApplicationContext, StarterManager, ServiceContext
- [Actor API](./actor.md) — DistributedActorSystem, LuaActor, ActorRegistry
- [服务层](../architecture.md#服务层-shieldservice) — send, call, query, timeout, fork
- [脚本 API](./script.md) — LuaVMPool, LuaServiceApi, shield.* 全局表
- [网络 API](./network.md) — Session, Reactor, ProtocolHandler
- [协议 API](./protocol.md) — BinaryProtocol, HttpRouter, WebSocketProtocolHandler
- [网关 API](./gateway.md) — GatewayRequestDispatcher, MiddlewareChain, GameGateway
- [服务发现 API](./discovery.md) — IServiceDiscovery, 多后端实现
- [序列化 API](./serialization.md) — JSON 序列化
