# API 说明

Shield 当前不再维护按模块展开的独立 API 手册。

原因很简单：旧版 API 文档和代码已经出现明显漂移，继续保留只会制造误导。后续以头文件、示例和测试作为一手来源，等接口边界稳定后再补回精简版 API 文档。

## 以这些位置为准

- `include/shield/core/` - 生命周期、`ApplicationContext`、`StarterManager`
- `include/shield/actor/` - Actor 系统、Lua Actor、分布式能力
- `include/shield/service/` - `send`、`call`、`query`、`timeout` 等服务语义
- `include/shield/script/` - Lua 运行时、VM 池、Lua API 绑定
- `include/shield/gateway/` - 网关、分发器、中间件、诊断
- `include/shield/protocol/` - TCP/HTTP/WebSocket/UDP/Schema 协议支持
- `include/shield/discovery/` - 服务发现抽象与后端实现
- `include/shield/config/` - 配置对象与配置管理

## 推荐阅读顺序

1. [快速开始](./quickstart.md)
2. [架构设计](./architecture.md)
3. [核心设计理念](./architecture-core-concepts.md)
4. [Skynet 对比](./skynet-comparison.md)
5. `include/` 与 `tests/`
