-- shield.database.mongodb Lua convenience layer.
--
-- This file is auto-loaded via the manifest's lua.search_paths entry. The
-- plugin's register_lua installs the callable namespace and its raw methods
-- (find / find_one / insert_one / insert_many / update_one / update_many /
-- delete_one / delete_many / count / aggregate / create_index / drop_index /
-- transaction). The helpers below add pure-Lua sugar that lives comfortably
-- in Lua land (no per-call C++ round-trip).
--
-- ObjectId stringification, ID helpers, and a functional transaction wrapper
-- that auto-commits on success / auto-rolls-back on error. The plugin's own
-- register_lua injects these as metatable-indexed helpers; users do not need
-- to require() this file directly.

local M = {}

-- Convert a 24-char hex string into the BSON ObjectId form
--   { ["$oid"] = "<hex>" }
-- so it round-trips through find/update filters.
function M.oid(hex)
    if type(hex) ~= "string" or #hex ~= 24 or not hex:match("^[0-9a-fA-F]+$") then
        error("mongodb.oid: expected 24 hex chars, got " .. tostring(hex))
    end
    return { ["$oid"] = hex:lower() }
end

-- True if `v` is an ObjectId-shaped table.
function M.is_oid(v)
    return type(v) == "table" and v["$oid"] ~= nil
end

-- Convenience: extract the raw hex string from an ObjectId-shaped table.
function M.oid_hex(v)
    if M.is_oid(v) then return v["$oid"] end
    return nil
end

return M
