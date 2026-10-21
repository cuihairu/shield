# 崩溃采集（Crashpad）

> 状态：**设计已定，实现分批落地**。选型 Google **Crashpad**（Breakpad 仅作
> 历史对照，不引入）。范围：进程崩溃（SIGSEGV / SIGABRT 等）采集为
> minidump **本地落盘** + 离线符号化还原调用栈。上传留位、默认不外发。

## 问题

runtime 崩溃（空指针、断言、内存破坏）只留下信号终止码时无法定位：
core dump 依赖 ulimit 与部署面配置，且不可控地巨大。需要一套随产物走、
崩溃时由**独立进程**写出紧凑 minidump、事后用符号离线还原调用栈的采集面。

## 选型

| 项 | 决策 |
| --- | --- |
| 框架 | Google **Crashpad**（Chromium/Android 同款），C++ 直集成 |
| 对照 | Breakpad 仅历史对照，不引入代码 |
| 接入方式 | **git 子模块**（vcpkg manifest 已冻结待裁决，不动；本机 vcpkg manifest install 是死路已核实） |
| 符号工具链 | rust `dump_syms` + `minidump-stackwalk`（装在构建机/CI，不进仓库依赖） |

Crashpad 关键能力正好对上需求：**out-of-process handler**（采集进程与
崩溃进程隔离，自身不易共崩）、minidump 格式与 Breakpad 兼容（dump_syms /
minidump-stackwalk 直接可用）、client 库 + handler 单独可执行的打包形态。

子模块三条 gitlink 全部登记在 shield 主仓，**扁平布局**（CI 一次
`submodules: recursive` 取齐）：

| 路径 | 源 | pin |
| --- | --- | --- |
| `third_party/crashpad` | github.com/chromium/crashpad | `677b9e33`（main tip） |
| `third_party/mini_chromium` | github.com/chromium/mini_chromium | `c0299833`（== crashpad DEPS pin） |
| `third_party/lss/lss` | chromium.googlesource.com/linux-syscall-support | `9719c1e1`（== crashpad DEPS pin，tag v2022.10.12） |

不用 crashpad DEPS 原生嵌套布局的原因：git 索引里 gitlink 是叶子，
`git submodule add` 拒绝子模块内部路径（已实测），嵌套 gitlink 无法
表达。crashpad 源码的 include 形态全部兼容扁平布局——`"base/…"`
（短形式，`-I third_party/mini_chromium` 解析）、`"third_party/lss/lss.h"`
（包装头在 crashpad 树内，`-I third_party/crashpad` 解析）、包装头展开的
`"third_party/lss/lss/linux_syscall_support.h"`（`-I <仓根>` 解析到
`third_party/lss/lss/`）。include 解析全部由 CMake 胶水（下文）接管，
不依赖 crashpad 自带构建。

## 架构

```
shield 进程                             crashpad_handler 进程（独立）
┌─────────────────────────┐            ┌──────────────────────────┐
│ crash::initialize()     │  spawn+pipe│ 等待崩溃事件             │
│  └ Crashpad Client ─────┼────────────┤                          │
│ 崩溃 → 信号/检查点 ─────┼────────────┤ 采内存快照写             │
│                         │            │  <dump_dir>/<uuid>.dmp   │
│ 启动扫描 <dump_dir> ────┼── 记 WARNING 日志（遗留 dump=上轮崩溃）│
└─────────────────────────┘            └──────────────────────────┘
        离线：dump_syms(二进制) → <name>.sym → minidump-stackwalk(dump)
```

## handler 初始化点

初始化序固定在 `bootstrap::initialize_impl()` 内（src/bootstrap/bootstrap.cpp）：

1. logger 初始化、config 文件加载完成（此后 `crash.*` 键可读）；
2. **config 校验与各子系统 starter 之前**插入 `crash::initialize()`——
   这是最早的"配置可用 + 日志可用"交汇点，bootstrap 全程都在其保护下；
3. 降级策略：handler 可执行找不到 / dump 目录建不了 → 记 **ERROR** 日志
   后**继续启动**（采集是增强项，绝不阻断服务）。

`--check-config` 语义是 "Initialize, validate, then shut down"，因此也会
经过初始化——顺带自检 handler 可执行与 dump 目录可写，随后 shutdown
正常回收 handler。

构建开关 `SHIELD_ENABLE_CRASHPAD`（**默认 ON**，产物带 handler）：

- 编译面：`src/crash/`、`include/shield/crash/` 的胶水代码与
  `#ifdef SHIELD_ENABLE_CRASHPAD` 分支全部随开关裁剪；
- **覆盖率门禁两个 job 显式 `-DSHIELD_ENABLE_CRASHPAD=OFF`**（沿用四开关
  OFF 的既有口径）：gcovr 过滤面与三项指标（98 line / 100 branch /
  100 function、optional line 100）与本批之前逐字节一致。

## dump 落盘目录

| 项 | 值 |
| --- | --- |
| 配置键 | `crash.dump_dir`，缺省 `""` → 相对 cwd 的 `crash/` |
| 容器内 | WORKDIR `/app` → `/app/crash`（目录随卷挂载即可外送采集） |
| 文件名 | `<uuid>.dmp`（crashpad database 自动命名） |
| 创建 | 初始化时创建（crashpad database Initialize），失败=降级 ERROR |
| 仓库 | `/crash/` 进 .gitignore（本地验收跑出的 dump 不脏 git status） |

启动扫描：`crash::initialize()` 枚举目录内既存 `*.dmp`，数量 >0 时记
`SHIELD_LOG_WARNING`（logger 名 `crash`）逐个列文件名——**上一轮崩溃的
遗留 dump 是运维告警的直接信号**（见「与日志/监控的打通」）。

## 独立 handler 进程打包

- CMake 目标：静态库 `crashpad_client` + 可执行 `crashpad_handler`
  （胶水 `cmake/crashpad.cmake`，源集按 Linux/POSIX 排除 win/mac/ios/tvos/
  fuchsia 与 `*_test.cc`，include 根 = crashpad、mini_chromium、仓根
  （lss 路径）+ crashpad 上游 compat 层（`compat/linux` 的 signal.h
  include_next shim 补 glibc 缺的 `SS_AUTODISARM`/`SA_EXPOSE_TAGBITS`、
  `compat/non_win` 提供 windows.h 形状的 minidump 头），define
  `CRASHPAD_LSS_SOURCE_EMBEDDED`；非 Linux 平台自动 OFF（mac/win 构建矩阵
  零回归，见「非目标」）。
- 产物布局：`build/bin/shield` 与 `build/bin/crashpad_handler` 同目录。
- 查找顺序：`crash.handler_path` 显式配置 → 缺省取 shield 自身可执行
  所在目录（`/proc/self/exe` 推导）拼 `crashpad_handler` → 都没有则降级
  ERROR（见初始化点）。
- Docker：builder 阶段产物随 `COPY . .` 编出，runtime 阶段
  `COPY --from=builder /build/build/bin/crashpad_handler /app/`，与
  `ENTRYPOINT /app/shield` 同目录；dump 目录默认 `/app/crash`。
- CI：**所有编译 C++ 的 job** checkout 加 `submodules: recursive`
  （docs job 纯 vitepress 构建不需要）。

## 符号表管理

原则：**符号与产物分离**——构建期生成并归档，运行现场不携带调试符号。

| 环节 | 做法 |
| --- | --- |
| 构建留 -g | 推荐 `RelWithDebInfo`（-O2 -g）；验收/CI job 用它构建 |
| 生成符号 | `dump_syms build/bin/shield > shield.sym`（DWARF → file+line 级） |
| 还原栈 | `minidump-stackwalk crash/<id>.dmp --symbols-path <sym目录>`（sym 需摆 breakpad 布局 `<module>/<debug-id>/<module>.sym`，脚本已代劳） |
| 一条命令 | `tools/symbolize-crash.sh build/bin/shield crash/<id>.dmp [sym目录]`（dump_syms + 摆布局 + stackwalk 一步完成） |
| Release 镜像 | 当前 Dockerfile Release 无 DWARF，但**有 symtab → 函数级调用栈仍可还原**；镜像保持不带符号（分离原则），需要 file+line 时用构建机 RelWithDebInfo 归档的 `.sym` |
| 归档 | `.sym` 按二进制 commit hash 命名归档（`SHIELD_GIT_COMMIT_HASH` 已注入版本串，dump 注解同样带版本，见下） |

minidump 与二进制的对应关系由两处保证：dump 内嵌模块 GUID/地址 +
启动时 annotation 写入版本串。

## 上传留位（默认关）

| 配置键 | 缺省 | 语义 |
| --- | --- | --- |
| `crash.upload_url` | `""` | **外发属数据外发，默认关**。本批不实现上传；置非空仅在启动时记 WARN「已配置但未实现，dump 仅本地」占位，防止误以为已上报 |

留位语义固定：将来实现上传时只允许显式配置开启，缺省永不联网。

## 故意崩溃验收（空指针开关）

- CLI 开关 `shield --crash-test`（无值）：完整初始化成功后先记一行
  ERROR 日志（崩溃前日志落盘），再故意空指针解引用
  （`crash_test_null_deref()`，独立函数便于符号化栈识别）。
- crashpad OFF 的构建（覆盖率 job 即是）：该 flag 解析为明确的
  「需要 crashpad 构建」错误并有测试覆盖（新分支全落在 OFF 面）。
- 验收断言链：
  1. 进程异常退出（信号终止）；
  2. `<dump_dir>` 出现 `*.dmp`；
  3. `dump_syms` + `minidump-stackwalk` 还原出的调用栈**含
     `crash_test_null_deref` 帧与 `shield::run` 帧**（符号化生效）；
  4. 启动日志出现上一轮遗留 dump 的 WARNING 扫描行（闭环到④）。
- CI 新增 crashpad job：RelWithDebInfo 构建 → 跑 `--crash-test` → 断言
  上述四条 → minidump + 栈输出作为 artifact 归档。

## 与既有日志/监控的打通

不新增监控端点，接入点全部走既有 logger（logger 名 `crash`、`bootstrap`）：

| 时机 | 级别 | 内容 / 告警用途 |
| --- | --- | --- |
| 初始化成功 | INFO | dump 目录、handler 路径（采集面就绪态） |
| 初始化降级 | ERROR | handler 缺失 / 目录不可建 → **采集已失效**，告警关键字 `crash collector degraded` |
| 启动扫描遗留 dump | WARNING | `pending crash dump` + 文件列表 → **上轮崩溃**，告警关键字 |
| upload_url 置空外非空 | WARN | 上传未实现占位 |
| 崩溃前（--crash-test 与真实崩溃同路径） | ERROR | 崩溃前最后一行日志与 dump 时间对齐 |

运维面：dump 目录可直接挂卷/打包回收；日志关键字即告警规则
（pending crash dump / collector degraded），与现有基于日志的监控同构。

## 配置参考

```yaml
crash:
  enabled: true           # 缺省 false；app.yaml 显式开（产物面默认带采集）
  dump_dir: ""            # 空 = ./crash（相对 cwd）
  handler_path: ""        # 空 = shield 同目录的 crashpad_handler
  upload_url: ""          # 留位：非空仅告警占位，不外发（数据外发默认关）
```

测试夹具配置不写 `crash:` 段 → 全部缺省 false：测试二进制零 handler
spawn，故意触发 fatal signal 的既有测试（si_addr 断言类）不受影响。

## 非目标

- 不做上传实现（留位如上）、不做符号服务器（`.sym` 文件归档即可）；
- 不做 Windows/macOS 验收（胶子按 POSIX 排除，Linux 先行）；
- 不采集 hang/超时（属于监控另一条线，互不越界）。
