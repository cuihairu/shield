-- echo.lua - 默认运行时的最小可观测服务
--
-- config/app.yaml 默认配置的 echo actor：一个无认证的 c2s `echo`
-- (route 1) → s2c `echo_result` (route 100) 闭环。新用户 `./build.sh run`
-- 之后立刻有端口可连（见 scripts/client_demo.py），不需要先读懂
-- hello_world 的认证/绑定流程。
--
-- 帧格式（idlen 封包）：[route_id:2B 大端][length:2B 大端][JSON body]，
-- body 是纯业务 JSON，不含路由字段。

local M = {
    connected = 0,
    echoed = 0,
}

function M.on_init(args)
    M.name = args.name or "echo"
    shield.log.info(M.name .. " started (c2s echo -> s2c echo_result)")
end

-- 连接建立：session 初始 target 即 echo
function M.on_client_bound(ctx, client)
    M.connected = M.connected + 1
    shield.log.info("client connected: " .. tostring(client:session_id()))
end

-- c2s RPC（route 1，requires_auth=false）。入站 fire-and-forget，
-- 响应必须经 s2c helper 显式发出。
function M.echo(ctx, client, request)
    M.echoed = M.echoed + 1
    local ok = shield.client_rpc.echo_result(client, {
        seq = M.echoed,
        data = request,
        time_ms = shield.now(),
    })
    if not ok then
        shield.log.warn("echo_result egress rejected (client gone?)")
    end
end

function M.on_disconnect(ctx, client, reason)
    M.connected = math.max(0, M.connected - 1)
    shield.log.info("client left: " .. tostring(client:session_id()) ..
                        " reason=" .. reason)
end

function M.on_exit(reason)
    shield.log.info("echo stopping: " .. reason)
end

return M
