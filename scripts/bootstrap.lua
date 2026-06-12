local M = {}

function M.on_init(args)
    shield.log.info((args.name or "bootstrap") .. " started")
end

return M
