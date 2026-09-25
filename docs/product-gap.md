# 产品化差距评估（Product Gap Review）

评估基准：**新用户 10 分钟跑通一个最小游戏服务端**（clone → 构建 → 启动 → 用客户端
打通一次请求）。对照对象：skynet（主要参照），以及同类开箱即用方案的开箱体验
（Nakama 的 `nakama new` + docker 一键起、Colyseus 的 `npm init` 模板路径）。

结论先行：**工程底座（配置校验、运维面、示例质量）高于预期，但"第一次跑通"路径
被三件事挡住——首次构建门槛、默认配置无可观测行为、没有模板工程生成**。这三件
正是本轮补齐的 top-3（见文末"已补齐项"）。

| 维度 | 现状评级 | 一句话结论 |
| --- | --- | --- |
| 一条命令起服务 | 🟡 半达标 | `./build.sh run` 存在，但首次构建要过 vcpkg 源码编译这道墙 |
| 默认配置零改动可跑 | 🟡 半达标 | 能跑但"跑了个寂寞"：无游戏端口，无可观测行为 |
| 示例/模板工程 | 🟡 半达标 | hello_world 示例质量高且有 e2e 测试；缺"生成我自己的工程" |
| 安装与部署脚本 | 🟢 基本达标 | build.sh/build.bat/Dockerfile 齐备；缺环境预检 |
| 日志与监控默认可用 | 🟢 达标 | console + /ops/* HTTP 默认开启，超出 skynet 默认水平 |
| 热更新与运维命令 | 🟡 部分 | 运维命令丰富；热更新只有设计稿（诚实标注） |
| 文档与上手路径 | 🟡 半达标 | 契约文档扎实；但 10 分钟线性路径不存在，tutorial 自称未验证 |
| 错误提示友好度 | 🟡 半达标 | 配置错误 fail-fast 优秀；环境错误裸奔 |

---

## 1. 一条命令能不能起服务

**现状**：`./build.sh run`（build.sh）一键 构建+启动 默认配置；Dockerfile 多阶段
构建并在构建期内跑 `--check-config` 自检。命令本身是达标的。

**差距**：命令前面的**首次构建**是真正的墙——

- vcpkg manifest 模式从源码编译 boost/CAF/openssl/lua/yaml-cpp 等，8 核机器
  也要几十分钟；弱网环境下 vcpkg 自身的 git clone 与工具下载（cmake tarball）
  都可能反复失败（本地实测：early EOF / SSL 35，需要断点续传兜底）。
- 环境要求没有预检：cmake ≥ 3.30、C++23 编译器（gcc ≥ 13 / clang ≥ 17 / MSVC
  19.38+）、ninja、VCPKG_ROOT。任一缺失时用户看到的是 cmake 原始报错，且
  vcpkg 失败会连带出"CMake was unable to find Ninja"这类**误导性二级错误**
  （本地实测，ninja 明明在 /usr/bin）。
- Docker 路径同样重（构建期内做整轮 vcpkg 编译），且 `EXPOSE 8080 8081 8082
  8083` 注释与默认配置实际行为不符（见下节）。

**skynet 对照**：`git clone && make && ./skynet examples/config.lua`——一个
make，一两分钟，无包管理器、无网络依赖（deps 佚在树内）。

**评级：挡路（top-3 之内，落在"启动脚本与文档"项）**。无法把依赖树砍到 skynet
那么小（CAF/asio 是既定架构决策），所以补齐方向是：build.sh 环境预检 + 可操作
的报错、文档写清前置要求矩阵与 vcpkg 二进制缓存（`VCPKG_DEFAULT_BINARY_CACHE`）
止损路径。

## 2. 默认配置是否零改动可跑

**现状**：`config/app.yaml` 零改动可启动：bootstrap actor（scripts/bootstrap.lua
只打一行"started"日志）+ console（/tmp/shield-console.sock）+ HTTP ops
（127.0.0.1:8080）。`--check-config` 离线校验通过，CI 有两条测试锚定它
（CMakeLists.txt:666,696）。

**差距**：

- **无可观测的游戏行为**。新用户跑起来后没有任何 TCP 端口可以连，第一个"看到
  它工作"的时刻必须切到 examples/hello_world（另一套 cmake 配置 + 另一份
  config）。默认路径与示例路径是断开的。
- Dockerfile 尾部注释 "Default ports: TCP 8080, UDP 8081, HTTP 8082, WS 8083"
  是过时的蓝图口径：默认配置没有 8080 TCP 游戏监听（8080 是 http ops，且绑定
  127.0.0.1——容器内绑定 127.0.0.1 意味着容器外不可达）。照注释 `docker run -p`
  会连不上，直接误导。

**评级：挡路（top-3 第一项）**。补齐方向：默认配置增加一个最小 echo 监听
（idlen 封包 + 一条 c2s/s2c 路由），让 `./build.sh run` 之后 `nc`/示例客户端
立刻有东西可连；同步修正 Dockerfile 注释与暴露口径。

## 3. 有没有可直接跑的示例/模板工程

**现状（示例）**：`examples/hello_world/` 是完整闭环——auth（TCP 监听 + 预登录）
→ player（bind 后单一 target）→ room（私有转发 + s2c 回包），带真实 TCP e2e
acceptance 测试（tests/hello_world_acceptance.cpp、
tests/acceptance/test_client_rpc_e2e.cpp）。示例质量高于 skynet examples 的
平均水平（skynet examples 多数不带测试）。

**现状（模板）**：**没有**。新用户开自己的工程需要：手抄 hello_world 的
main.cpp + 自己写 CMake 链接 shield + 拷 config/scripts——每一步都在劝退。
skynet 生态里"从 examples 拷一个 config 改改"是心智默认；Nakama 有
`nakama new`，Colyseus 有 `npm init colyseus`——同类产品都把脚手架当标配。

**评级：挡路（top-3 第二项）**。补齐方向：`scripts/new_project.sh <dir>` 一条
命令生成可 `--check-config` 通过、可构建、可启动的最小工程（模板即从已验证的
hello_world 派生），并用 ctest 锚定"生成的工程配置必须能通过校验"。

## 4. 安装与部署脚本

**现状**：build.sh（debug/release/run/clean 四模式）、build.bat、多阶段
Dockerfile（构建期自检 --check-config，运行期非 root 用户，这两点是好实践）。

**差距**：Linux 裸机前置依赖清单只存在于 Dockerfile 的 apt 命令里，新用户要
自己去挖；没有 docker-compose / 没有预检脚本。

**评级：次要（并入 top-3 的脚本与文档项一并补）**。

## 5. 日志与监控默认开箱可用

**现状**：这是**超出预期的一项**。默认配置 console 日志开启；HTTP ops 默认开启
且路由齐全：`/ops/health` `/ops/status` `/ops/metrics` `/ops/services[/:name]`
`/ops/plugins` `/ops/config`，另有显式 opt-in 的 `/ops/eval` `/ops/profile`
（src/console/ops_http_handler.cpp:163-221）；Unix console 默认开启，命令面覆盖
`help` / `root.status` / `root.services` / `root.log.level`（运行时可调）/ 
`attach`（交互 REPL）/ `eval` / `lua.inspect` / `lua.snapshot` / `lua.diff`
（src/console/root_commands.cpp、lua_commands.cpp）。skynet 默认只有一个
debug console（node 监听）。

**注意点**：默认绑定 127.0.0.1 / Unix socket 是正确的安全默认，但容器与远程
部署文档必须写明怎么改；metrics 内容口径文档尚缺。

**评级：达标**。本轮只补"文档里把已有能力说清楚"，不加新代码。

## 6. 热更新与运维命令

**现状**：运维命令丰富（见上节，attach REPL + 快照/diff 是 skynet 没有的）。
热更新只有设计稿（blue-green service replacement，docs/runtime-lua-vm.md），
没有 `reload` 类命令。

**skynet 对照**：`clear` 机制 + data-driven 代码加载是 skynet 的招牌能力；
Shield 的 one-VM-per-service 模型在原理上不需要原地打补丁（换服务实例即可），
但 blue-green 未落地前，这个差距是真实存在的。

**评级：欠缺但已诚实标注**（roadmap 有位次，不藏）。**不进 top-3**——新用户
前 10 分钟不需要热更新，先不为此写代码。

## 7. 文档与上手路径

**现状**：文档量大且契约优先（架构/Lua API/配置/错误码/插件 ABI 均有权威文
档），quickstart.md 诚实区分"目标体验"与"当前状态"。

**差距**：

- **10 分钟线性路径不存在**。quickstart 是"状态报告"体，不是"跟我做"体；
  验证命令分散在"当前状态/验收标准"两节，还要求读者自己拼出 examples 的
  构建命令。
- `docs/tutorial-game-backend.md` 开头自曝 "not a currently verified runnable
  guide"，其 YAML 与 hello_world 已验证配置的语义有出入（binding/route 写法
  未对齐）。对新人这是陷阱：照 tutorial 抄会抄出跑不起的东西。
- 没有客户端样例。hello_world 的 e2e 是 C++ 写的；新用户没有现成的东西去
  "连一下试试"（一个 20 行的 python idlen 客户端就够）。

**评级：挡路（top-3 第三项）**。补齐方向：quickstart 重写为线性 10 分钟路径
（前置环境 → 构建 → 启动 → 连接验证，全部命令实测）；tutorial 改为指向已验证
路径或修正为可跑；补 scripts/client_demo.py。

## 8. 错误提示是否对新人友好

**现状**：**配置侧优秀**——启动期 fail-fast，逐键报错，`--check-config` 可离
线校验，CI 锚定；Lua 脚本加载错误走保护路径带 traceback（load_script 已修，
NDEBUG 下不再直达 abort）。

**差距**：**环境侧裸奔**——cmake 版本不够、编译器不支持 C++23、VCPKG_ROOT 未
设、网络抓包失败，全都以工具链原始报错砸到用户脸上，且互相级联（见第 1 节的
Ninja 误报）。

**评级：半达标**。补齐方向：build.sh 预检（版本探测 + 每项缺失给出"装什么"
的一句话指引），并入 top-3 第三项。

---

## 优先级排序（按"10 分钟跑通"目标）

| 序 | 项 | 理由 |
| --- | --- | --- |
| P0-1 | 默认配置可观测（echo 监听） | 没有它，"跑通"无从定义 |
| P0-2 | 模板工程一条命令生成 | 新用户从"看示例"到"写自己的"唯一桥梁 |
| P0-3 | 启动脚本与文档线性化（预检 + quickstart 重写 + tutorial 修正 + 客户端样例 + Dockerfile 口径修正） | 把前两步串成一条实测过的路 |
| P1 | 部署文档（容器端口口径、二进制缓存）、metrics 口径文档 | 止损，不挡 10 分钟 |
| P2 | 热更新 blue-green 落地 | skynet 对标项，另立项（见 architecture-review.md 路线） |

## 已补齐项（本轮落地）

- [x] 默认配置增加 echo 闭环：`config/app.yaml` 新增 `echo` actor
      （scripts/echo.lua，TCP 0.0.0.0:8001，idlen + json，c2s `echo` id=1 /
      s2c `echo_result` id=100，`requires_auth: false`）——`./build.sh run`
      后即可连接。CI 既有两条 `--check-config` 锚定默认配置，新增
      `shield_default_config_boot_smoke` 实际拉起默认配置验证监听可连。
- [x] 模板工程：`scripts/new_project.sh <dir>` 从已验证模板生成最小工程
      （config + scripts + main.cpp + CMakeLists），生成后自动跑
      `shield --check-config` 自检；`tests/new_project_scaffold.cpp` 锚定
      "生成的工程必须通过配置校验 + 文件齐全"。
- [x] build.sh 环境预检：cmake ≥ 3.30 / C++23 编译器 / ninja / VCPKG_ROOT
      逐项探测，缺失时给"装什么"的一句话指引，不再让 vcpkg 二级错误背锅。
- [x] quickstart.md 重写为线性 10 分钟路径（每条命令实测）；
      tutorial-game-backend.md 修正为指向已验证路径；Dockerfile 过时的
      EXPOSE 注释与实际口径对齐。
- [x] `scripts/client_demo.py`：20 行 idlen+json 客户端，打通 echo 与
      hello_world 的 login→move 闭环，作为"连接验证"步骤的默认工具。
