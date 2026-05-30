# 生产部署

本文档介绍将 Shield 部署到生产环境的方式。

## Docker 部署

项目内置 `Dockerfile`，支持多阶段构建：

```bash
docker build -t shield:latest .
docker run -d -p 8080:8080 -p 8082:8082 -p 8083:8083 shield:latest
```

### Docker Compose 示例

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

  etcd:
    image: quay.io/coreos/etcd:v3.5.7
    command: etcd --listen-client-urls http://0.0.0.0:2379 --advertise-client-urls http://etcd:2379
    ports:
      - "2379:2379"
```

## 多节点部署

参考 `templates/multi-node/` 目录：

- **gateway.yaml** — 网关节点，监听 TCP/HTTP/WS/UDP
- **logic.yaml** — 逻辑节点，运行 Lua 业务脚本

网关节点通过服务发现找到逻辑节点，消息通过 CAF Actor 系统传递。

## Kubernetes 部署

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: shield-gateway
spec:
  replicas: 3
  template:
    spec:
      containers:
      - name: shield
        image: shield:latest
        ports:
        - containerPort: 8080
          name: tcp
        - containerPort: 8082
          name: http
        - containerPort: 8083
          name: websocket
        livenessProbe:
          httpGet:
            path: /health
            port: 8082
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /health
            port: 8082
```

## 端口规划

| 端口 | 协议 | 用途 |
|------|------|------|
| 8080 | TCP | 游戏客户端 TCP 连接 |
| 8082 | HTTP | HTTP API + 健康检查 |
| 8083 | WebSocket | WebSocket 连接 |
| 8084 | UDP | UDP 连接 |
| 9090 | HTTP | Prometheus 指标（可选） |
| 13000 | TCP | 调试控制台 |

## 健康检查

```bash
# 基础健康检查
curl http://localhost:8082/health

# 详细状态
curl http://localhost:8082/health/detailed

# 运行时状态
curl http://localhost:8082/status
```

## 构建脚本

项目提供一键构建脚本：

```bash
# Linux/macOS
./build.sh release

# Windows
build.bat release

# 运行
./build.sh run --config config/app.yaml
```

## Nginx 负载均衡

```nginx
upstream shield_http {
    least_conn;
    server gateway1:8082;
    server gateway2:8082;
}

upstream shield_ws {
    ip_hash;  # WebSocket 需要会话保持
    server gateway1:8083;
    server gateway2:8083;
}
```
