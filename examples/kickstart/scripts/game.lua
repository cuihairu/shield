-- Shield Kickstart - 业务脚本
--
-- 演示三件事：
--   1. on_init 里建表（非协程上下文 = 同步回退路径，返回形状一致）
--   2. 定时器回调（协程派发）里走 DB 异步挂起路径
--   3. 业务状态经 SQLite 持久化，进程重启不丢
--
-- 服务脚本是模块：定义 on_init/on_shutdown 后 return M（hello_world 同形）。
-- 改动本文件后重启进程即生效（v1 语义：脚本随进程启动编译）。
-- 试着把 HEARTBEAT_NOTE 改成别的词，重启后在日志里看到新值。

local HEARTBEAT_NOTE = "alive"

local db = shield.database.sqlite("database.default")

local M = {}

local function on_heartbeat()
    -- 协程上下文：execute 在插件 worker 线程执行，本协程挂起让位
    local ok, result = db:execute(
        "INSERT INTO heartbeat (ts, note) VALUES (?, ?)", { os.time(), HEARTBEAT_NOTE })
    if not ok then
        shield.log.error("heartbeat insert failed: " .. tostring(result and result.code))
        return
    end
    shield.log.info("heartbeat #" .. tostring(result.last_insert_id)
                        .. " (" .. HEARTBEAT_NOTE .. ")")
end

function M.on_init(args)
    local ok, err = db:execute([[
        CREATE TABLE IF NOT EXISTS heartbeat (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            ts   INTEGER NOT NULL,
            note TEXT
        )]])
    if not ok then
        shield.log.error("create table failed: " .. tostring(err and err.code))
        return false, "create table failed"  -- on_init 失败：service 不进入 running
    end
    shield.timer(5000, on_heartbeat)
    shield.log.info("kickstart game service ready (db bound, name="
                        .. tostring(args and args.name) .. ")")
    return true
end

function M.on_shutdown()
    shield.log.info("kickstart game service stopping")
end

return M
