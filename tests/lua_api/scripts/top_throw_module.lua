-- Negative fixture: throws at module top-level load time.
-- Used by LAPI-001-04 to verify the loader surfaces script_load_failed.
error("intentional_top_level_error")
local M = {}
return M
