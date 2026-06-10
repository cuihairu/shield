# UDP 支持

UDP 属于重构后 `net` 模块的目标能力之一，但本文不再描述旧架构中的 metrics、monitoring 或独立 protocol 层设计。

## 目标边界

`net` 负责：

- UDP socket 生命周期。
- 数据包收发。
- session 识别策略。
- 超时和清理。
- 将原始数据交给 transport 或 Lua gateway。

`net` 不负责：

- Prometheus 指标。
- 服务发现。
- 业务可靠传输策略。
- 框架级中间件。

## Transport

如果项目需要可靠 UDP、加密、压缩或私有包格式，应通过 C++ `transport` 扩展点实现。

```text
UDP packet → net → transport → Lua gateway
```

## 后续需要补充

- session key 设计。
- 包大小限制。
- 丢包和重放策略。
- transport 与 Lua gateway 的消息格式。
- 对应单元测试和压力测试。
