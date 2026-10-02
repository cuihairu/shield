# Shield 配置系统与配置更新机制分析

> 对照真实代码的分析（2026-10-02，基线 c02a887）。所有结论均给出 `文件:行` 证据；未实现的能力明确标注"不存在"，不写推测。

## 0. 结论摘要

Shield 的配置模型是**启动时一次性装配**：YAML 多文件深合并 → 全量校验 → 派发到各子系统，之后进程内配置**只读**。

| 用户关心的问题 | 现状 |
|---|---|
| 配置从哪来 | 磁盘 YAML（`config/app*.yaml`）+ CLI 覆盖（`--config` 多文件按序、`--node-id` 注入） |
| 怎么下发/热更新 | **没有热更新**。`reload_config()` 是桩，恒返回 `true`（src/config/config.cpp:1796） |
| 版本管理 | **没有配置版本号/hash/世代**。`app.version`、`server_manager.info.version` 是业务自报展示字段，不参与变更比对 |
| 客户端怎么应用 | **不适用**。传输层/gateway 无任何配置推送消息；游戏客户端 SDK 是独立实现、不在本仓库（docs/engine-sdk-design.md） |
| 失败回滚 | 启动期失败 = 拒绝启动（`return false`，进程退出，无部分启动）；运行期配置不可变，无需回滚 |

## 1. 模块清单

| 模块 | 职责 | 关键代码 |
|---|---|---|
| shield_config | YAML 加载/深合并/点分键存储/校验/actor 声明解析 | src/config/config.cpp、include/shield/config/config.hpp |
| shield_bootstrap | 装配序：加载 → 校验 → 日志/插件/集群/player/server → actors 派发 | src/bootstrap/bootstrap.cpp:409-1050 |
| shield_lua（Lua API） | 只读读取面 `shield.config` / `shield.player.config` / `shield.server.config` | src/lua/lua_api.cpp:1346、2445、2934 |
| shield_console / ops | `root.config` 命令、`GET /ops/config?key=` | src/console/root_commands.cpp:342、src/console/ops_http_handler.cpp:912 |
| shield_player / shield_server | 各自 `from_global_config` 快照一份启动参数 | src/player/player_manager.cpp:36、src/server/server_manager.cpp:70 |
| shield_cluster | `parse_cluster_config()` 启动期解析 | src/cluster/cluster_manager.cpp:22 |
| shield_plugin | `load_plugin_config()` 从全局配置解析插件段 | src/plugin/plugin_config.cpp:83 |

## 2. 数据流（启动期，一次性）

```
config/app*.yaml (磁盘)
      │  CLI: --config a.yaml --config b.yaml   （bootstrap.cpp:409-424，后文件覆盖先文件）
      ▼
Config::load_yaml ──► merge_yaml_nodes 递归合并（config.cpp:44-64）
      │                非 map 节点（标量/数组）整体替换
      ▼
flatten_yaml_node → 点分键哈希表（读 shared_lock / 写 unique_lock）
      │  CLI: --node-id → global_config().set("cluster.node_id")   （bootstrap.cpp:427，全仓唯一生产期写）
      ▼
validate_runtime_config（config.cpp:1115-1638）
      │  失败 → SHIELD_LOG_ERROR + return false → 进程退出
      ▼
派发（全部启动时一次读定）：
  ├─ 日志级别/sinks（bootstrap.cpp:431-457）
  ├─ PluginHost::startup（bootstrap.cpp:494-501）
  ├─ ClusterManager（bootstrap.cpp:504-516）
  ├─ PlayerConfig::from_global_config（player_manager.cpp:36）
  ├─ ServerManager config（server_manager.cpp:70）
  ├─ Lua 运行参数（有消费方的键）：sandbox.allow_os/allow_io（lua_runtime.cpp:202-204）、
  │    module_path（lua_runtime.cpp:222）、cache.*（lua_runtime.cpp:268-272）、
  │    script_path（bootstrap.cpp:85-86 脚本解析链）
  ├─ lua.vm.*（mode/max_vms/max_memory_mb）：**无运行期消费方**（见坑 9）
  └─ runtime_actors()（config.cpp:1645）
       ├─ 每个 actor → 服务 spawn：opts = {name, args, config=actor.options_json}
       │    （bootstrap.cpp:975-985 → lua_service.cpp:2224-2227 取 opts["config"] 传给脚本）
       ├─ network.* → 监听器参数（tcp、envelope/codec、限流、blocklist、TLS）
       └─ rpc.routes 跨 actor 合并去重 → RpcDescriptorTable（bootstrap.cpp:910-960，
            id/name 冲突在 spawn 前失败）
```

## 3. 运行期读取面（全部只读）

| 面 | 形式 | 证据 |
|---|---|---|
| C++ 任意位置 | `shield::config::get/get_int/get_bool`（shared_lock 活读——若未来出现写者，读方立即可见） | config.cpp:1802-1818 |
| Lua 服务 | `shield.config(key[, default])`：字符串按 true/false/float/int 探测转换，缺键返回 default 或 nil | lua_api.cpp:1346-1420 |
| Lua 服务（模块快照） | `shield.player.config()`、`shield.server.config()`：启动时快照的子集表 | lua_api.cpp:2445、2934 |
| 服务私有配置 | spawn opts 的 `config` 键 = 该 actor 的 `options:` 子树（YAML→JSON） | bootstrap.cpp:979-985、lua_service.cpp:2227 |
| console | `root.config [key]`（缺 key dump 全量 JSON） | root_commands.cpp:342-346 |
| ops HTTP | `GET /ops/config?key=...`（**缺 key 不 dump 全量**，只回提示串，代码注释自认 simplified） | ops_http_handler.cpp:912-955 |

写路径全仓仅一处生产代码：`bootstrap.cpp:427` 的 `cluster.node_id` CLI 注入（`Config::set` 经 unique_lock）。Lua/ops/console 均未暴露 setter。

## 4. 各专题回答

### 4.1 配置从哪来
- 文件：`config/{app,app-dev,app-prod,app-with-redis,app-with-sqlite}.yaml`。
- CLI 多文件按序叠加，**后覆盖先**（bootstrap.cpp:409-424）；加载即深合并进全局单例，`reset_config()` 清空后重装（bootstrap.cpp:411）。
- 合并语义（config.cpp:44-64）：两侧都是 map 时按键递归；任一侧非 map（含数组）→ **overlay 整体替换**。
- 类型系统：`ConfigValue = variant<string,int64,double,bool,vector<string>>`（config.hpp:16），flatten 后以点分键存哈希表；`get_string` 会把数字转字符串（config.cpp:918-927），`to_json` 输出**扁平点分键** JSON（嵌套结构用 `subtree_json(config, path)`，config.hpp:144）。

### 4.2 下发/热更新
- **不存在热更新**：`reload_config()` 恒 `return true`，注释自述"Would need to track the original config path"（config.cpp:1796-1799）；生产代码零调用方，仅测试 `ReloadConfigReturnsTrue` 断言桩行为（tests/coverage/test_cov_config.cpp:1165-1168）。
- 无文件 watch、无热重载信号——进程信号面只有 SIGINT/SIGTERM（仅用于停机，src/shield.cpp:51-65），全仓无 SIGHUP/SIGUSR 处理。实证：`grep -rn "SIGHUP\|SIGUSR" src/ --include="*.cpp" --include="*.hpp" --include="*.h"` 返回无命中。改配置的现行唯一途径 = 改文件 + 重启进程。
- 跨节点也无配置同步：cluster 消息类型仅 Hello/HelloAck/Heartbeat/Routes/Envelope（include/shield/cluster/cluster_messages.hpp:31-85）。其中 RoutesMsg 是服务路由表随心跳的同步，属运行期服务发现而非配置分发；节点超时判定见 cluster_manager.cpp:58-75。实证：`grep -rn "config\|Config" include/shield/cluster/cluster_messages.hpp` 返回无配置相关消息类型。全仓不存在面向配置的集群消息。

### 4.3 版本管理
- 无配置内容 hash、无世代/单调版本号、无变更通知。全局配置对象上没有任何版本字段。
- 现存的"version"字段均为业务展示：
  - `app.version`（yaml 自报，如 "0.1.0"）；
  - `server_manager.info.version` → `ServerManager::version()`，空则回退编译期 fallback，用于 server 列表/ops 展示（server_manager.cpp:253-258）；
  - ops `/info` 的 node version 字段。
- 一次性派生的结构（actor 声明、RPC descriptor 表）在启动时物化后与 YAML 再无关联——运行期改磁盘文件对进程零影响。

### 4.4 客户端怎么应用
- 不适用：transport/net/session/gateway 代码中无任何面向客户端的配置消息（`EnvelopeConfig` 是协议编解码参数，启动期固定，src/transport/protocol.cpp:483-517）。
- 业务若需"给客户端下发配置"，现行做法是在服务里用数据面（`shield.data`/global data）+ RPC 自行实现——那是数据同步，不是配置系统的一部分。

### 4.5 失败回滚
- **启动期**（唯一存在失败处理的阶段）：
  - 文件打不开/解析异常 → `load_yaml` 返回 false → bootstrap `return false`（bootstrap.cpp:419-424，config.cpp:870-907）；
  - 校验失败（模块段 vs 编译开关、app.name、log.level、actors 全 schema、rpc id/name 唯一、脚本文件存在性）→ `return false`（bootstrap.cpp:482-486；错误串见 config.cpp:213-811、1142-1180）；
  - rpc 冲突在**任何 actor spawn 之前**拒绝（bootstrap.cpp:954-960）——不会出现半启动拓扑。
  - 因为失败即退出、成功后不可变，"回滚"退化为"进程未启动"，无需补偿逻辑。
- **运行期**：配置不可变 → 无需回滚。唯一隐患是 `reload_config()` 假成功（见坑 2）。

## 5. 坑清单（全部对照代码）

1. **多文件叠加时数组整体替换**：两份配置都有 `actors:` 列表时是覆盖不是拼接（config.cpp:57-58 非 map 即 `Clone(overlay)`）。想"追加一个 actor"必须复制完整列表。限流/blocklist 等数组同理。
2. **`reload_config()` 假成功**：恒 true（config.cpp:1796）。未来若把它接到 ops/console 当热更新开关，会得到"报告成功但什么都没发生"。
3. **`reset_config()` 悬空引用**：helper 内 reset 会销毁调用方持有的 `Config&`（已知测试坑，shield-config-test-pitfall 记忆）。
4. **类型往返有损**：YAML 数字 flatten 后 `get_string` 转字符串，`shield.config` Lua 侧再按字符串探测转 bool/number（lua_api.cpp:1357-1395）：大 int64、前导零的数字串（如 `"01"`）、`e/E` 出现在普通字符串里都会走 float 探测路径。需要精确类型时应走 `subtree_json`。
5. **`/ops/config` 缺 key 不返回全量**：只回 "Use ?key=..." 提示（ops_http_handler.cpp:932-935，注释自认 simplified）；全量 dump 用 console 的 `root.config`。
6. **`source_dir` 被最后加载文件覆盖**：`impl_->source_dir` 每次 `load_yaml` 重写（config.cpp:885-892），actor 脚本相对路径解析 `existing_script_path` 跟随**最后**一份配置文件的目录（config.cpp:762-763、1593）——多文件叠加 + 相对 script 路径时容易踩。
7. **校验只发生在启动**：`Config::set` 无校验路径（唯一调用方是受控的 node_id 注入）；若未来暴露 Lua/ops 写入口，校验缺口会立即出现。
8. **插件段不进 flatten 消费**：`load_plugin_config()` 直接读 `Config` 对象（plugin_config.cpp:83-86），同样启动期一次读定；插件 DLL 内部另有 manifest `schema_version`（必须为 1，src/plugin/manifest.cpp:77-81）——那是插件 ABI 版本，与配置版本无关。
9. **`lua.vm.*` 仅在启动时校验**：`lua.vm.mode` 只在启动时校验"必须是 per_service"（config.cpp:1189-1205），仅作为启动合法性检查；`lua.vm.max_vms` / `max_memory_mb` 在配置层被读取并记录在 YAML 中，但在运行期零消费方——不参与任何 VM 资源限制（grep src/ 实测，零命中）。三个键的改动不改变任何运行期行为；VM 资源上限目前不由配置驱动，上限由 CAF 运行时自行管理。实证：`grep -rn "lua.vm.max_vms\\|lua.vm.max_memory_mb" src/ --include="*.cpp" --include="*.hpp" --include="*.h"` 返回无命中。

## 6. 若要演进（现状依据，非建议实施）

按现有代码结构，配置热更新需要补的最小面是：`Config` 记住来源路径列表（桩注释已指出）、reload 走"装载到影子 Config → 校验 → 换 root/原子替换 storage"两阶段（读方 shared_lock 已具备活读语义），以及给一次性派生结构（actors/RPC 表/监听器参数）定义重建策略——第三点在当前"启动即物化"的架构下成本最高，也是该框架选择不可变配置的根本原因。
