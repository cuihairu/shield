-- Test service for LAPI-010 (shield.config read API).
--
-- Exposes the shield.config reader to the C++ test harness so the value
-- parsing rules (bool / int / double / string / missing default) can be
-- verified against the global runtime config.

local M = {}

function M.read(key)
    return shield.config(key)
end

function M.read_default(key, default)
    return shield.config(key, default)
end

return M
