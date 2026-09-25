-- echo.lua - 最小业务服务：把 c2s echo 原样回给 s2c echo_result
--
-- handler 形态固定为 handler(ctx, client, request)；入站是
-- fire-and-forget，响应必须经 s2c helper（shield.client_rpc.<name>）
-- 显式发出。

local M = {
    echoed = 0,
}

function M.on_init(args)
    M.name = args.name or "echo"
    shield.log.info(M.name .. " started")
end

function M.echo(ctx, client, request)
    M.echoed = M.echoed + 1
    local ok = shield.client_rpc.echo_result(client, {
        seq = M.echoed,
        data = request,
    })
    if not ok then
        shield.log.warn("echo_result egress rejected (client gone?)")
    end
end

function M.on_exit(reason)
    shield.log.info("echo stopping: " .. reason)
end

return M
