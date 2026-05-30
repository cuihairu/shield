# 开发路线图

Shield 的开发按 7 个阶段推进，全部已完成。

## 已完成阶段

### Phase 1: 边界重置

定义 Shield 核心边界，分离 core 和 extensions，精简 ApplicationContext 职责，标记核心/启动器/插件模块。

### Phase 2: Skynet 语义层

在 CAF 之上实现 Skynet 风格的服务 API：send、call、timeout、sleep、fork、query、uniqueservice。添加 ServiceContext（thread-local + RAII Guard）和调试控制台。

### Phase 3: Lua 运行时

标准化 Lua 服务入口点（on_init + on_message），通过 `shield.*` 全局表暴露运行时 API。实现 LuaServiceApi（sol2 绑定）、LuaServiceBase（C++ 侧包装）、shield_service.lua（Lua 侧基类）。支持热重载。

### Phase 4: 网络运行时

统一协议请求/响应模型（GatewayRequest / GatewayResponse），实现中间件管道（MiddlewareChain + 内置 logging/cors/auth），WebSocket 作为一流协议路径，游戏网关模板（登录/会话/消息分发）。

### Phase 5: 开箱即用

单节点和多节点配置模板，gateway + logic + storage 参考布局，一键构建脚本（build.sh / build.bat），多阶段 Dockerfile，跨平台构建说明。

### Phase 6: 可观测性

RuntimeDiagnostics（HTTP 端点查询运行时状态），健康检查，Prometheus 指标，配置重载规则，分层日志（服务/协议/运行时）。

### Phase 7: 可选扩展

数据库访问抽象保持可选，插件系统与核心运行时隔离，高级 DI/IoC 不扩展运行时表面，数据访问功能不进入最小启动路径。

## 文档

- VitePress 文档站点
- Skynet 对比页、CAF 映射页
- 快速开始指南、游戏后端教程
- API 参考（actor、script、protocol、discovery、gateway、core）

## 后续可能方向

以下不在当前路线图中，仅作参考：

- HTTPS/TLS 支持
- Protobuf 序列化
- HTTP/2
- 集群管理 UI
- 性能基准测试和调优
