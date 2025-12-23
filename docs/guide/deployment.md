# 部署指南

## 构建发布版本

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 目录结构

```
shield/
├── bin/              # 可执行文件
│   └── shield
├── lib/              # 库文件
│   └── libshield_core.a
├── config/           # 配置文件
│   └── shield.yaml
├── scripts/          # Lua 脚本
│   └── player_actor.lua
└── logs/             # 日志目录
```

## 环境变量

```bash
export SHIELD_CONFIG_PATH=/etc/shield/config.yaml
export SHIELD_LOG_PATH=/var/log/shield
export SHIELD_SCRIPT_PATH=/opt/shield/scripts
export SHIELD_PLUGIN_PATH=/opt/shield/plugins
```

## Systemd 服务

创建 `/etc/systemd/system/shield.service`:

```ini
[Unit]
Description=Shield Game Server
After=network.target

[Service]
Type=simple
User=shield
Group=shield
WorkingDirectory=/opt/shield
ExecStart=/opt/shield/bin/shield --config /etc/shield/shield.yaml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

启动服务：

```bash
sudo systemctl daemon-reload
sudo systemctl enable shield
sudo systemctl start shield
sudo systemctl status shield
```

## Docker

### Dockerfile

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    liblua5.4-dev \
    libssl-dev

WORKDIR /opt/shield
COPY . /opt/shield

RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

EXPOSE 8080 8081

CMD ["./bin/shield", "--config", "config/shield.yaml"]
```

### docker-compose.yml

```yaml
version: '3.8'

services:
  shield:
    build: .
    ports:
      - "8080:8080"
      - "8081:8081"
    volumes:
      - ./config:/opt/shield/config
      - ./scripts:/opt/shield/scripts
      - ./logs:/opt/shield/logs

  nacos:
    image: nacos/nacos-server:latest
    ports:
      - "8848:8848"
    environment:
      MODE: standalone
```

## 监控

### 健康检查

```cpp
#include <shield/health/health_check.hpp>

auto& registry = shield::health::HealthCheckRegistry::instance();
auto status = registry.get_status("gateway");
```

### Prometheus 指标

```cpp
#include <shield/metrics/prometheus_service.hpp>

shield::metrics::PrometheusConfig config;
config.endpoint = "/metrics";
config.port = 9090;

shield::metrics::PrometheusService service(config);
service.start();
```

访问 `http://localhost:9090/metrics` 获取指标。

## 日志

日志配置示例：

```yaml
log:
  global_level: info
  file: logs/shield.log
  max_size: 100M
  max_files: 10
  pattern: "[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v"
```

查看日志：

```bash
tail -f logs/shield.log
grep ERROR logs/shield.log
```
