-- Parent service exercising lua_table_to_json's shape detection through the
-- spawn opts table: a non-array table with an int key <= 0, an int key in
-- object position, and a key that is neither string nor int.
local M = {}

function M.on_init(args)
    local a = args.args or {}
    local h, err = shield.spawn(a.child_script, {
        name = 'opts_shape_child',
        [0] = 'zero',
        [2] = 'two',
        [true] = 'boolean key',
        config = {x = 1},
    })
    if h then
        M.result = 'spawned'
    else
        M.result = 'rejected: ' .. tostring(err and err.code or err)
    end
    return true
end

function M.get_result()
    return M.result
end


return M
