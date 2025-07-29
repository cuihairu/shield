# 构建项目

### CMake 构建选项

```bash
# 调试构建 (默认)
cmake -DCMAKE_BUILD_TYPE=Debug

# 发布构建
cmake -DCMAKE_BUILD_TYPE=Release

# 带调试信息的发布构建
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 启用编译命令导出 (IDE 支持)
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 静态链接
cmake -DBUILD_SHARED_LIBS=OFF

# 启用详细输出
cmake -DCMAKE_VERBOSE_MAKEFILE=ON
```

### Shield 特定选项

```bash
# 禁用测试构建
cmake -DSHIELD_BUILD_TESTS=OFF

# 启用示例构建
cmake -DSHIELD_BUILD_EXAMPLES=ON

# 启用性能分析
cmake -DSHIELD_ENABLE_PROFILING=ON

# 启用内存检查
cmake -DSHIELD_ENABLE_SANITIZERS=ON
```