# 部署

本文只描述重构后的目标部署边界。

## 当前定位

Shield core 的目标是单节点优先运行时。因此当前 core 部署文档不把多节点部署、服务发现、Prometheus、健康检查或集群编排写成默认能力。

多进程/多机器部署属于 `shield_cluster` 官方可选模块的范围。集群模块稳定前，生产环境可以先使用外部进程管理、Kubernetes Service、静态配置或业务层路由组织多实例。

## 目标运行方式

实现稳定后，单节点服务应类似：

```bash
./build/bin/hello_world --config config/app.yaml
```

或使用统一入口：

```bash
./build/bin/shield --config config/app.yaml
```

具体命令要等 `shield::run(argc, argv)` 和最终 CLI 决策完成后冻结。

## 生产建议

- 使用操作系统服务管理进程生命周期。
- 使用外部日志采集系统处理日志。
- 使用外部 sidecar 或平台能力做健康检查。
- 在 `shield_cluster` 稳定前，使用业务层逻辑或外部基础设施处理多节点。

## 非目标

以下旧文档内容不再属于 core 部署方案，后续若保留应归入 `shield_cluster` 或 `shield_ops`：

- Docker Compose 中内置 etcd / redis 服务发现集群。
- 多节点 gateway / logic / storage 模板作为框架默认能力。
- `/health` 和 `/metrics` 端口约定。
- Prometheus scrape 配置。
