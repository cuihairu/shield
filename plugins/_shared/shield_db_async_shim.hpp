// Shared async-entry Lua shim (docs/db-async-design.md). Every DB driver
// plugin installs the same wrapper over its instance proxy: query/query_one/
// execute become submit-and-yield wrappers around the driver's __sync_*
// implementations. Kept in one place so the three drivers cannot drift.
//
// Contract with the installing plugin:
//   proxy.__db_submit(method, sql, params) -> session id (0 = run sync)
//   proxy.__sync_query / __sync_query_one / __sync_execute
// Both call shapes work — colon (db:query(sql, params)) and dot
// (db.query(sql, params)). session == 0 (async disabled, main thread, host
// without the resume slots) falls back to the synchronous implementation
// inline, so callers always get the documented (ok, ...) contract.
//
// The completion payload is the VALUES array only — the host's
// resume_suspended_caller pushes the ok boolean itself, so the shim sees
// (ok, result...) from [result] and (false, err_table) from [{code, message}].

#pragma once

namespace shield::plugins {

inline constexpr const char* kDbAsyncShimLua = R"lua(
local proxy = ...
local submit = proxy.__db_submit
local function wrap(method)
    local sync = proxy['__sync_' .. method]
    proxy[method] = function(a, b, c)
        local sql, params
        if a == proxy then
            sql, params = b, c
        else
            sql, params = a, b
        end
        local session = submit(method, sql, params)
        if session == 0 then
            return sync(sql, params)
        end
        local r = table.pack(coroutine.yield())
        if not r[1] then
            return false, r[2]
        end
        return true, table.unpack(r, 2, r.n)
    end
end
wrap('query')
wrap('query_one')
wrap('execute')
return proxy
)lua";

}  // namespace shield::plugins
