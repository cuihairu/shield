-- Child whose on_init always fails (spawn rollback tests).

local M = {}

function M.on_init(args)
    return nil, "boom"
end

return M
