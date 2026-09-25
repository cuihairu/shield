# <APP_NAME>

由 `scripts/new_project.sh` 生成的最小 Shield 游戏服务端工程。

## 结构

```text
<APP_NAME>/
├── config/app.yaml    # 服务声明：actor + 监听端口 + RPC 路由
├── scripts/echo.lua   # 业务：c2s echo → s2c echo_result
└── README.md
```

## 运行

```bash
# 在 shield 仓库根目录（需先完成 shield 构建，见其 docs/quickstart.md）
./build/bin/shield --config <本工程目录>/config/app.yaml
```

## 连接验证

shield 仓库根目录的 `scripts/client_demo.py`：

```bash
python3 scripts/client_demo.py --port 8100 --message '{"hello":"world"}'
```

## 改成自己的玩法

1. 在 `config/app.yaml` 的 `actors[].rpc.routes` 里声明新的 c2s/s2c
   路由（route id 全局唯一；`requires_auth: true` 的路由需要先经
   `shield.client.bind` 认证切换 target——参考 examples/hello_world）。
2. 在 `scripts/echo.lua` 里加同名 handler（binding 名）。
3. 完整示例（认证/绑定/转发）见 shield 仓库 `examples/hello_world/`。

## 嵌入自己的 C++ 入口（可选）

参考 shield 仓库 `examples/hello_world/main.cpp`（两行 main）。
