# 常见问题

## 编译相关

### Q: vcpkg 依赖安装失败

```bash
cd $VCPKG_ROOT && git pull && ./bootstrap-vcpkg.sh
```

### Q: CMake 找不到 C++20 编译器

确保编译器版本足够：
- GCC 11+
- Clang 14+
- MSVC 2019 16.8+

### Q: Windows 上编译报错

确保设置了 `VCPKG_ROOT` 环境变量，并使用 `build.bat` 脚本构建。

## 运行相关

### Q: 端口被占用

修改 `config/app.yaml` 中的端口号：

```yaml
gateway:
  listener:
    port: 9090
  http:
    port: 9092
  websocket:
    port: 9093
```

### Q: Lua 脚本加载失败

检查 `lua.script_dir` 配置路径是否正确，脚本文件是否存在。

### Q: 健康检查返回错误

```bash
curl http://localhost:8082/health
curl http://localhost:8082/status
```

查看日志输出确认服务是否启动成功。

## 架构相关

### Q: Shield 和 Skynet 的关系

Shield 以 Skynet 为设计参考，提供 Skynet 风格的服务 API（send/call/query），但使用 CAF 作为 Actor 传输层，并内置了 HTTP/WebSocket/UDP、服务发现、可观测性等 Skynet 没有内置的功能。详见 [Skynet 对比](skynet-comparison.md)。

### Q: CAF 提供了什么

CAF 处理 Actor 的底层机制：spawn、send、request、schedule、序列化、分布式连接。Shield 在 CAF 之上添加了游戏服务器语义。详见 [CAF 映射](caf-mapping.md)。

### Q: 如何创建一个 Lua 服务

创建 `.lua` 文件，实现 `on_init()` 和 `on_message(msg)` 函数，使用 `shield.*` API 访问运行时。详见 [快速开始](quickstart.md) 和 [游戏后端教程](tutorial-game-backend.md)。

### Q: 如何启用 Prometheus 指标

```yaml
metrics:
  enabled: true
  port: 9090
```

或启动参数 `--enable-metrics`。
