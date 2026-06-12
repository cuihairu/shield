local M = {}

function M.on_init(args)
    M.greeting = args.args.greeting
end

function M.echo(value)
    return M.greeting .. ":" .. value, shield.sender()
end

function M.mark(value)
    M.marked_value = value
end

function M.get_marked()
    return M.marked_value
end

return M
