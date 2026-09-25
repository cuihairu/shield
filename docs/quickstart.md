# 快速上手

目标：**新用户 10 分钟内从 clone 跑到一次真实的客户端 RPC 回包**。
下面是一条线性路径，每一步都有预期结果；出问题时先看文末「常见问题」。

## 0. 前置环境（5 分钟内）

| 依赖 | 版本 | 检查命令 |
| --- | --- | --- |
| CMake | ≥ 3.30 | `cmake --version` |
| C++ 编译器 | gcc ≥ 13 / clang ≥ 17 / MSVC 19.38+ | `g++ --version` |
| vcpkg | 任意近期版 | `echo $VCPKG_ROOT` |
| python3 | ≥ 3.6（客户端演示用） | `python3 --version` |

没有 vcpkg 的话：

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
```

Ubuntu 缺基础工具：`sudo apt install -y build-essential cmake ninja-build
curl zip unzip tar pkg-config python3`。

`./build.sh` 开头会做同样的环境预检，缺什么会给一行修复指引。

## 1. 构建（首次 20–40 分钟）

```bash
./build.sh release
```

首次构建会由 vcpkg 从源码编译依赖（boost/CAF/openssl/lua 等），耗时主要
在这里；之后增量构建只要几十秒。想让多台机器/多次 clone 复用编译产物：

```bash
export VCPKG_DEFAULT_BINARY_CACHE=~/.cache/vcpkg-binary
mkdir -p $VCPKG_DEFAULT_BINARY_CACHE
```

## 2. 启动默认服务

```bash
./build/bin/shield --config config/app.yaml
```

看到 `echo started` 即成功。默认配置提供了：

| 能力 | 地址 | 说明 |
| --- | --- | --- |
| echo 游戏监听 | `0.0.0.0:7900` | 无认证 c2s `echo` → s2c `echo_result` |
| HTTP 运维面 | `127.0.0.1:8080` | `curl 127.0.0.1:8080/ops/health` |
| console | `/tmp/shield-console.sock` | `help` 查看全部命令 |

## 3. 连接验证（新开一个终端）

```bash
python3 scripts/client_demo.py
```

预期输出（route 100 = `echo_result`，data 原样回显）：

```text
reply route=100 body={"seq":1,"data":{"hello":"shield"},"time_ms":...}
```

帧格式（idlen 封包）：`[route_id:2B 大端][length:2B 大端][JSON body]`，
body 是纯业务 JSON，不含路由字段——`scripts/client_demo.py` 全文不到
100 行，可直接作为客户端协议参考。

## 4. 生成自己的工程（一条命令）

```bash
./scripts/new_project.sh ~/my_game
./build/bin/shield --config ~/my_game/config/app.yaml &
python3 scripts/client_demo.py --port 8100
```

脚手架内容：`config/app.yaml`（echo 监听 8001）+ `scripts/echo.lua` +
README。生成时自动跑 `--check-config` 自检。

## 5. 进阶：完整客户端 RPC 闭环

`examples/hello_world/` 是带认证/绑定/转发的完整示例（auth → player →
room），构建方式：

```bash
cmake -B build-examples -S . -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -DSHIELD_BUILD_TESTS=OFF -DSHIELD_BUILD_EXAMPLES=ON
cmake --build build-examples --target hello_world
./build-examples/bin/hello_world --config examples/hello_world/config/app.yaml
python3 scripts/client_demo.py --port 8100 --message '{"player_id":"p1"}'
```

登录回包是 route 100 `login_result`。示例带真实 TCP e2e 测试
（`tests/acceptance/test_client_rpc_e2e.cpp`）。

## 6. 改成自己的玩法

1. 在 `actors[].rpc.routes` 声明路由（id 全局唯一；c2s/s2c 各一条）；
2. 在 Lua 脚本里写同名 handler：`function M.<binding>(ctx, client, req)`，
   回包用 `shield.client_rpc.<s2c_name>(client, table)`；
3. `shield --check-config --config <你的配置>` 离线校验后再启动；
4. 需要认证切换目标服务时用 `shield.client.bind`（hello_world 的
   auth.lua 是最短示例）。

Lua 侧可用能力（send/call/sleep/timer/fork/config/log…）见
[Lua API 契约](lua-api.md)；配置语义见[配置运行时语义](runtime-config.md)。

## 常见问题

- **vcpkg 下载依赖失败（SSL/early EOF）**：网络抖动，直接重跑
  `./build.sh release`（已下载的包会命中缓存）；配
  `VCPKG_DEFAULT_BINARY_CACHE` 更稳。
- **7900/8001 端口被占**：改配置里的 `network.tcp` 再启动。
- **cmake/编译器版本不够**：见第 0 步的版本要求；`pip install --user
  "cmake>=3.30"` 是不改系统安装新版 cmake 的捷径。
- **Windows**：用 `build.bat`（或 VS 直接开 CMake 项目），vcpkg 同样
  通过 `VCPKG_ROOT` 提供；路径示例按 Windows 风格调整。

## 设计目标与现状口径

最终目标是用户只需要一个 C++ 入口（`shield::run(argc, argv)`）+ 一份
`app.yaml`；上文的 `shield` 二进制与脚手架就是这个形态的最小实现。
配置错误启动期 fail fast；`--check-config` 可离线校验；Lua service 文件
必须返回 table；`shield.call`/`shield.sleep`/timer/客户端 RPC handler
均为协程化路径，可在其中 sleep/call 而不阻塞 actor。
