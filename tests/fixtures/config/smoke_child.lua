local M = {}

function M.on_init(args)
    M.greeting = args.args.greeting
end

function M.echo(ctx, value)
    return M.greeting .. ":" .. value, ctx.sender
end

function M.mark(ctx, value)
    M.marked_value = value
end

function M.get_marked(ctx)
    return M.marked_value
end

return M
