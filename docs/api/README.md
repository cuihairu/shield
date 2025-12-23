# API 参考

Shield C++ API 参考文档。

## 版本信息

当前版本: **0.1.0**

## 命名空间

所有公共 API 位于 `shield` 命名空间下。

```cpp
#include <shield/version.hpp>

std::cout << shield::get_version_string() << std::endl;
// Output: Shield Game Framework v0.1.0 (Git Commit: xxx)
```

## 模块索引

- [核心 API](./core.md) - ApplicationContext, Service, PluginManager
- [Actor API](./actor.md) - ActorSystem, LuaActor, ActorRegistry
- [脚本 API](./script.md) - LuaEngine, LuaVMPool
- [网络 API](./network.md) - Session, Reactor, ProtocolHandler

## 类型定义

```cpp
namespace shield {
    // 版本
    constexpr const char* VERSION = "0.1.0";
    constexpr int VERSION_MAJOR = 0;
    constexpr int VERSION_MINOR = 1;
    constexpr int VERSION_PATCH = 0;

    // 获取版本字符串
    std::string get_version_string();
}
```
