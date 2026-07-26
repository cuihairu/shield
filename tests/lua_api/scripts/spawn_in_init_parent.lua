-- Parent that spawns a child synchronously inside on_init (main-thread path
-- of shield.spawn).

local M = {}

local child_id = nil
local child_node = nil

function M.on_init(args)
    local a = args.args or {}
    local h, err = shield.spawn(a.child_script, { name = a.child_name })
    if h then
        child_id = h:id()
        child_node = h:node()
    end
    return true
end

function M.get_child()
    return child_id, child_node
end

return M
