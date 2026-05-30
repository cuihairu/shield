# 性能调优

Shield 基于 C++20 和 CAF Actor 模型，性能主要由以下因素决定：Actor 调度效率、网络 I/O 吞吐、Lua VM 执行速度。

## CAF Actor 调度

CAF 内置工作窃取调度器，默认配置已适合大多数场景。调优方向：

- 增加 Actor 并行度：拆分高负载服务为多个 Actor 实例
- 减少 `call()` 阻塞时间：优先使用 `send()` 异步消息
- 避免在 Lua `on_message()` 中执行耗时计算

## 网络优化

### 网关线程配置

```yaml
gateway:
  threading:
    io_threads: 8   # 建议 CPU 核心数
```

- TCP: MasterReactor + SlaveReactor 池，连接均匀分配
- HTTP: Boost.Beast，支持 Keep-Alive
- WebSocket: 独立端口，不与 TCP/HTTP 竞争

### 中间件开销

内置中间件（logging、cors）开销极低。`auth_middleware` 的性能取决于验证函数实现。

## Lua VM 调优

- Lua VM 池复用 VM 实例，避免频繁创建/销毁
- `shield.call()` 内部使用 `caf::scoped_actor` 同步请求，适合低延迟场景
- 预加载脚本（`preload_scripts`）减少首次请求延迟

```yaml
lua:
  script_dir: "scripts/"
  auto_reload: true
  preload_scripts:
    - "init.lua"
```

## 编译优化

```bash
# 发布构建
./build.sh release

# 或手动 CMake
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native"
```

## 监控

通过 RuntimeDiagnostics 查看实时状态：

```bash
curl http://localhost:8082/status
```

启用 Prometheus 指标收集 HTTP 延迟分布和 Actor 消息计数：

```yaml
metrics:
  enabled: true
  port: 9090
```
