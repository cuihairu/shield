# Shield Kickstart — 10 分钟跑通全链路

四件套：**Lua 业务脚本 + SQLite 持久化 + health.http 健康端点 + metrics.prometheus 指标端点**。
C++ 只有一行启动入口（`main.cpp`），其余全部是配置与 Lua。

## 三步跑起来

```bash
# 1. configure：开启三个插件一起构建（默认全关）
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DSHIELD_BUILD_DB_PLUGIN_SQLITE=ON \
      -DSHIELD_BUILD_PLUGIN_HEALTH=ON \
      -DSHIELD_BUILD_PLUGIN_METRIC=ON

# 2. build：kickstart 可执行文件 + 插件包（产出到 build/bin/plugins/<package.id>/）
cmake --build build --parallel --target kickstart shield_database_sqlite \
      shield_health_http shield_metric_prometheus

# 3. run（从仓库根目录；plugins.directory 相对当前工作目录解析）
./build/bin/kickstart --config examples/kickstart/config/app.yaml
```

启动后日志里会看到 `kickstart game service ready (db bound)`，随后每 5 秒一条
`heartbeat #N (alive)`。

## 验证全链路

```bash
# 健康探针（health.http，K8s 兼容）
curl -s http://127.0.0.1:8086/health
curl -s http://127.0.0.1:8086/ready

# Prometheus 指标（metrics.prometheus）
curl -s http://127.0.0.1:8087/metrics | head
# 空 body 属预期：本示例没有指标注册者（shield.metrics.v1 的 Lua 面
# 规划中，见 docs/plugin-system.md 插件矩阵）；端点本身已验证可达。

# 数据落库验证：Ctrl-C 停掉进程后看文件
ls -la kickstart.db   # SQLite 文件在运行目录（plugins.directory 同级的 CWD）
```

## 改 Lua 验证业务链路

把 `scripts/game.lua` 顶部的 `HEARTBEAT_NOTE = "alive"` 改成别的词，
重启进程：日志立即出现新词，且 `heartbeat` 表行数继续增长（SQLite 数据
不随进程消失）。v1 语义是脚本随进程启动编译——改脚本重启生效；
运行期动态性用 `shield.spawn` / `shield.call`（见 docs/lua-api.md）。

## 这份配置各自在做什么

| 配置段 | 作用 |
| --- | --- |
| `actors[].script` | 业务入口；`on_init` 建表 + `shield.timer` 每 5s 心跳写库 |
| `plugins.instances` | 每个插件包一个实例：`package` 指向 `bin/plugins/` 下被 CMake 产出的包，`config` 进 manifest 的 `config_schema` 校验 |
| `plugins.bindings` | 逻辑名 → 实例：Lua 用 `shield.database.sqlite("database.default")` 访问，不出现实例 id |
| 端口 8086 / 8087 | health.http 与 metrics.prometheus 各自内嵌的 HTTP 监听（boost::beast，独立于业务面） |

进阶：插件发现/自动装配（`plugins.packages` 意图级配置）、脚手架
`tools/new_plugin.sh`、生命周期与 manifest 字段表见
[docs/plugin-system.md](../../docs/plugin-system.md)。
