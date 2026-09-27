// Shared async-entry Lua shim (docs/db-async-design.md). Every DB driver
// plugin installs the same wrapper over its instance proxy: query/query_one/
// execute become submit-and-yield wrappers around the driver's __sync_*
// implementations, and transaction() is orchestrated in Lua so the body's
// statements suspend too. Kept in one place so the three drivers cannot drift.
//
// Contract with the installing plugin:
//   proxy.__db_submit(method, sql, params, token) -> session id (0 = sync)
//   proxy.__sync_query / __sync_query_one / __sync_execute
//   proxy.__sync_transaction(callback)
// Both call shapes work — colon (db:query(sql, params)) and dot
// (db.query(sql, params)). session == 0 (async disabled, main thread, host
// without the resume slots) falls back to the synchronous implementation
// inline, so callers always get the documented (ok, ...) contract.
//
// Async transaction protocol (M4): __tx_begin acquires a connection and opens
// the transaction on a worker, resuming with a token; __tx_query/__tx_query_
// one/__tx_execute run on that held connection; __tx_commit/__tx_rollback
// finish and release it. The token-carrying tasks run on the driver's
// serialized tx lane, so a caller timeout that queues a rollback behind a
// still-running statement can never put two workers on one connection.
//
// Hard rule (db-discipline.md): inside a transaction body only the tx handle's
// SQL methods may yield. Pool-level db:query/query_one/execute and nested
// db:transaction from the same coroutine raise immediately — the runtime teeth
// for the rule the design doc enforces at review level for shield.call/sleep.
//
// The completion payload is the VALUES array only — the host's
// resume_suspended_caller pushes the ok boolean itself, so the shim sees
// (ok, result...) from [result] and (false, err_table) from [{code, message}].

#pragma once

namespace shield::plugins {

inline constexpr const char* kDbAsyncShimLua = R"lua(
local proxy = ...
local submit = proxy.__db_submit

-- Coroutines currently inside a transaction body (weak-keyed so an abandoned
-- coroutine never pins the flag). VM-global on purpose: a tx body must not
-- yield on ANY db instance's pool entry, not just the one it opened on.
local function in_tx_body()
    local open = rawget(_G, '__shield_db_tx_open')
    return open ~= nil and open[coroutine.running()] == true
end

local function call(method, sql, params, token)
    -- One submit-and-yield round trip. ok == nil means the host has no async
    -- entry for this call and the caller must use its synchronous form.
    local session = submit(method, sql, params, token)
    if session == 0 then return nil end
    local r = table.pack(coroutine.yield())
    if not r[1] then return false, r[2] end
    return true, table.unpack(r, 2, r.n)
end

local function wrap(method)
    local sync = proxy['__sync_' .. method]
    proxy[method] = function(a, b, c)
        local sql, params
        if a == proxy then
            sql, params = b, c
        else
            sql, params = a, b
        end
        if in_tx_body() then
            error('db: ' .. method .. ' inside a transaction body is ' ..
                      'forbidden; use the tx handle (docs/db-discipline.md)', 2)
        end
        local ok, x, y = call(method, sql, params, 0)
        if ok == nil then return sync(sql, params) end
        return ok, x, y
    end
end
wrap('query')
wrap('query_one')
wrap('execute')

proxy.transaction = function(a, b)
    local cb = (a == proxy) and b or a
    if in_tx_body() then
        error('db: nested transaction is forbidden (docs/db-discipline.md)', 2)
    end
    local ok, token = call('__tx_begin', nil, nil, 0)
    if ok == nil then return proxy.__sync_transaction(cb) end
    if not ok then return false, token end

    local open = rawget(_G, '__shield_db_tx_open')
    if open == nil then
        open = setmetatable({}, {__mode = 'k'})
        rawset(_G, '__shield_db_tx_open', open)
    end
    local co = coroutine.running()
    open[co] = true

    -- tx handle: same (ok, result) / (false, err) shapes as the pool entry,
    -- but every statement runs on the connection this transaction holds.
    local tx = {}
    local function tx_method(method)
        return function(_, sql, params)
            local ok2, x, y = call('__tx_' .. method, sql, params, token)
            if ok2 == nil then
                error('db: transaction statement cannot run synchronously', 2)
            end
            return ok2, x, y
        end
    end
    tx.query = tx_method('query')
    tx.query_one = tx_method('query_one')
    tx.execute = tx_method('execute')

    -- pcall is yieldable in Lua 5.2+, so the body's tx statements suspend
    -- straight through it while a body error still lands in res[1].
    local res = table.pack(pcall(cb, tx))
    open[co] = nil
    local commit = res[1] and res[2] ~= false
    local ok3, err3 = call(commit and '__tx_commit' or '__tx_rollback',
                           nil, nil, token)
    if not ok3 then return false, err3 end
    if not res[1] then
        return false, {
            code = 'transaction_rolled_back',
            message = 'callback raised an error',
        }
    end
    if res[2] == false then
        return false, {
            code = 'transaction_rolled_back',
            message = 'callback returned false',
        }
    end
    return true, table.unpack(res, 2, res.n)
end

return proxy
)lua";

}  // namespace shield::plugins
