-- Negative fixture: deliberate syntax error.
-- Used by LAPI-001-03 to verify the loader surfaces script_load_failed.
local M = {}
function M.on_init(args -- missing closing paren
    return true
end
