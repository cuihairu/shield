# 常见问题

## 编译问题

### Q: CMake 找不到 CAF

**A**: 确保设置了 `CMAKE_TOOLCHAIN_FILE` 指向 vcpkg：

```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### Q: 链接错误 undefined reference

**A**: 检查是否安装了所有必需的依赖库，并确保 vcpkg triplet 正确。

## 运行问题

### Q: 启动时找不到配置文件

**A**: 使用 `--config` 参数指定配置文件路径，或确保 `config/shield.yaml` 存在。

### Q: Lua 脚本加载失败

**A**:
1. 检查脚本路径是否正确
2. 确保脚本语法正确
3. 查看日志中的详细错误信息

## 性能问题

### Q: 如何调整 Actor 线程数

**A**: 修改配置文件中的 `actor.worker_threads`，建议设置为 CPU 核心数。

### Q: 如何优化网络性能

**A**:
1. 增加 `slave_reactor_threads` 数量
2. 调整 TCP 缓冲区大小
3. 考虑使用 UDP 替代 TCP

## 开发问题

### Q: 如何调试 Lua Actor

**A**: 在 Lua 脚本中使用 `log_info` 输出调试信息，设置日志级别为 DEBUG。

### Q: 如何添加自定义协议

**A**: 继承 `shield::protocol::ProtocolHandler` 并实现相应接口。
