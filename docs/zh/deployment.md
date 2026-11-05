# 部署

单节点优先运行时的部署口径：构建产物、容器、进程管理、优雅停机与观测接入。
多节点属于 `shield_cluster` 官方可选模块（见 [集群语义](runtime-cluster.md)），
本文不展开。

## 构建产物

```bash
./build.sh release          # 产物 build/bin/shield（build.sh 固定用 build/ 目录）
./build/bin/shield --check-config --config config/app.yaml   # 部署前自检
```

- 统一入口是 `shield` 可执行文件；`--config` 缺省读 `config/app.yaml`，
  可重复传多个配置按序合并（CLI 契约见
  [启动流程运行时语义](runtime-bootstrap.md)）。
- 部署前必跑 `--check-config`：它执行**完整** bootstrap（服务 spawn、端口
  绑定）再 shutdown，能暴露脚本缺失、端口占用、binding 悬空这类 YAML 校验
  查不出的问题。失败即拒绝部署。
- 依赖安装慢的网络环境：把 vcpkg 二进制缓存指到持久卷，重装不再重编译——
  ```bash
  export VCPKG_DEFAULT_BINARY_CACHE=/var/cache/shield/vcpkg
  ```

## 运维控制台（FIX-WEB-04 / FIX-WEB-14）

浏览器端运维控制台（`console/`，React + Vite）在生产**不是** dev server：
构建一次，静态产物直接由 shield 进程经 `http.static_root` 托管：

```bash
# 1. 构建前端（需 pnpm；CI `console-build` job 会构建并以
#    `shield-console-dist` artifact 交付）。
cd console && pnpm install --frozen-lockfile && pnpm run build && cd ..

# 2. 把 shield 的 HTTP ops 监听指向构建产物。路径原样使用，须传绝对目录。
cat >> config/app.yaml <<'YAML'
http:
  static_root: /opt/shield/console/dist
YAML

# 3. ops API 仍走 Bearer 门；SPA 壳与静态资源公开，浏览器可加载应用与登录门。
./build/bin/shield --config config/app.yaml
```

- **无 dev server**：生产路径由 shield HTTP 监听直接托管 `console/dist`
  （同源）。`vite dev` / `vite preview` 仅限本地开发，勿对外暴露。
- **SPA 深链**：未知非 API 路径回退 `index.html`，`/services`、`/config`
  等直刷正常；未知 `/ops/*` API 路径返回 JSON `404`，绝不返回 HTML 壳。
- **产物无秘密**：CI `console-build` job 扫描构建产物 JS/CSS/HTML 中
  的秘密串（私钥、AWS/gh/sk token、bearer token、api_key/password/secret
  赋值），命中即构建失败。秘密一律不进前端。
- **开发代理**：`vite.config.ts` 仅在本地开发把 `/ops` 代理到
  `http://127.0.0.1:8080`；生产产物不含代理目标（同源托管）。

## 容器

仓库自带多阶段 Dockerfile（构建期跑 `--check-config` 自检，运行期非 root）：

```bash
docker build -t my-shield .
docker run -p 7900:7900 my-shield
```

端口口径（与仓库默认 config/app.yaml 对齐）：

| 端口 | 用途 | 发布策略 |
|------|------|---------|
| 7900 | 游戏客户端 TCP（echo 入口，idlen+json） | 对外发布 |
| 8080 | HTTP ops（`/ops/health` `/ops/metrics` …） | **不发布**，容器内 `127.0.0.1` 供探针/sidecar 抓取 |
| console | Unix socket `/tmp/shield-console.sock` | 不经网络暴露 |

把 ops 改成对外可抓：显式把 `ops` 的 listen 地址改为 `0.0.0.0:8080`（或按
部署反代），并自行加网络层访问控制——HTTP ops 的安全口径见
[运维运行时语义](runtime-ops.md)。

## 进程管理（systemd）

SIGINT/SIGTERM 进程内统一走协作式停机（`shield::request_stop()` →
shutdown 分段 drain）。停机窗口用 `TimeoutStopSec` 给足，不要缩短 kill
超时打断 drain（存档写一半比多等几秒严重）：

```ini
# /etc/systemd/system/shield.service
[Unit]
Description=Shield game server
After=network-online.target

[Service]
User=shield
WorkingDirectory=/opt/shield
ExecStartPre=/opt/shield/bin/shield --check-config --config /opt/shield/config/app.yaml
ExecStart=/opt/shield/bin/shield --config /opt/shield/config/app.yaml
Restart=on-failure
# 给足 config/app.yaml 里 shutdown.timeout.total（默认 60s）的停机窗口
TimeoutStopSec=70

[Install]
WantedBy=multi-user.target
```

`ExecStartPre` 复用 `--check-config`，配置坏了连服务都不会进启动流程。

## 优雅停机语义

停机顺序与预算由配置显式声明（`shutdown.timeout`，默认值）：

| 分段 | 默认 | 含义 |
|------|------|------|
| `service_drain` | 30000ms | 服务 drain 在途消息（玩家存档落盘等） |
| `service_stop` | 10000ms | 服务退出 |
| `plugin_shutdown` | 10000ms | 插件关停（连接池 drain） |
| `total` | 60000ms | 总预算；systemd `TimeoutStopSec` 应大于它 |

业务侧的停机钩子（`on_exit` / server 状态机 `on_server_state_change`）在这
些窗口内执行；窗口调大意味着更长的发布滚动时间，按存档耗时定。

## 观测接入

- 探活：`GET /ops/health`（进程内直读，actor 网格卡死仍应答）。
- 指标：`GET /ops/metrics` Prometheus 0.0.4 文本；指标口径与抓取语义见
  [运维运行时语义](runtime-ops.md)。建议抓取间隔 ≥5s。
- 诊断：Unix socket console（`attach` REPL、`lua.inspect`/`snapshot`/`diff`），
  见 [诊断控制台](diagnostics-console.md)；生产建议只留 socket 方式。

## 多实例

单机多实例 = 多份配置（错开游戏端口与 console socket 路径），由外部进程
管理器组织；跨节点协作等 `shield_cluster`。集群稳定前的多节点组织方式
（外部进程管理 / Kubernetes Service / 业务层路由）见
[开放决策](open-decisions.md)。

## 非目标

- Docker Compose 内置服务发现集群、多节点 gateway/logic/storage 模板——
  归 `shield_cluster`。
- Prometheus scrape 配置模板——指标格式已对齐，接入属部署方监控栈。
