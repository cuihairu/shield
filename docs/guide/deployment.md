# 部署指南

## 构建发布版本

```bash
./build.sh release           # Linux/macOS
# build.bat release           # Windows
```

## 运行

```bash
./build/bin/shield server --config config/app.yaml
```

## 目录结构

```
shield/
├── build/bin/         # 可执行文件 (shield)
├── config/            # 配置文件 (app.yaml)
├── scripts/           # Lua 脚本
├── logs/              # 日志目录
└── templates/         # 项目模板
```

## 环境变量

```bash
export VCPKG_ROOT=/path/to/vcpkg
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
WorkingDirectory=/opt/shield
ExecStart=/opt/shield/bin/shield server --config /etc/shield/app.yaml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

## Docker

项目内置多阶段 `Dockerfile`：

```bash
docker build -t shield:latest .
docker run -d -p 8080:8080 -p 8082:8082 -p 8083:8083 shield:latest
```

### Docker Compose

```yaml
version: '3.8'

services:
  shield-gateway:
    build: .
    command: shield server --config /etc/shield/gateway.yaml
    ports:
      - "8080:8080"   # TCP
      - "8082:8082"   # HTTP
      - "8083:8083"   # WebSocket
    volumes:
      - ./config:/etc/shield
      - ./scripts:/app/scripts

  shield-logic:
    build: .
    command: shield server --config /etc/shield/logic.yaml
    volumes:
      - ./config:/etc/shield
      - ./scripts:/app/scripts
```

## 端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 8080 | TCP | 游戏客户端 TCP 连接 |
| 8082 | HTTP | HTTP API + 健康检查 |
| 8083 | WebSocket | WebSocket 连接 |
| 8084 | UDP | UDP 连接 |
| 13000 | TCP | 调试控制台 |

## 健康检查

```bash
curl http://localhost:8082/health
```

## 日志

```bash
tail -f logs/shield.log
```

## 监控

启用 Prometheus 指标：

```yaml
metrics:
  enabled: true
  port: 9090
```

访问 `http://localhost:9090/metrics`。
