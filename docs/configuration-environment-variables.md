# 环境变量

Shield 支持通过环境变量覆盖配置：

```bash
# 基础配置
export SHIELD_CONFIG_FILE="/path/to/config.yaml"
export SHIELD_LOG_LEVEL="debug"
export SHIELD_NODE_ID="server-01"

# 网络配置
export SHIELD_GATEWAY_HOST="0.0.0.0"
export SHIELD_GATEWAY_PORT="8080"
export SHIELD_HTTP_PORT="8081"
export SHIELD_WEBSOCKET_PORT="8082"

# 服务发现
export SHIELD_DISCOVERY_TYPE="etcd"
export SHIELD_ETCD_ENDPOINTS="http://etcd1:2379,http://etcd2:2379"
export SHIELD_ETCD_USERNAME="shield"
export SHIELD_ETCD_PASSWORD="password"

# 数据库
export SHIELD_DB_HOST="localhost"
export SHIELD_DB_PORT="5432"
export SHIELD_DB_NAME="shield_game"
export SHIELD_DB_USER="shield_user"
export SHIELD_DB_PASS="shield_pass"

# Redis
export SHIELD_REDIS_HOST="localhost"
export SHIELD_REDIS_PORT="6379"
export SHIELD_REDIS_DB="0"
```