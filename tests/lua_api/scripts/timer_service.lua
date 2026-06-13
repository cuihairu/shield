-- Test service for LAPI-007 (Timers and Tasks)

local M = {}

-- Track timer callbacks
local timer_callbacks = {}
local timer_counter = 0

-- Track tasks
local tasks = {}
local task_counter = 0

function M.on_init(args)
    M.test_case = args.config and args.config.test_case or "default"
end

function M.get_timer_callbacks()
    return timer_callbacks
end

function M.clear_timer_callbacks()
    timer_callbacks = {}
end

function M.timer_callback(value)
    table.insert(timer_callbacks, {
        type = "timer",
        value = value,
        timestamp = shield.now()
    })
end

function M.get_tasks()
    return tasks
end

function M.clear_tasks()
    tasks = {}
end

function M.task_callback(value)
    table.insert(tasks, {
        type = "task",
        value = value,
        timestamp = shield.now()
    })
end

function M.throwing_callback()
    error("timer_callback_error")
end

function M.sleep_callback(duration)
    shield.sleep(duration)
    return "slept_" .. duration
end

function M.long_running_task()
    -- Simulate long work
    local iterations = 0
    for i = 1, 1000000 do
        iterations = iterations + 1
    end
    return iterations
end

-- Test timer that cancels itself
function M.self_canceling_timer()
    timer_counter = timer_counter + 1
    local timer_id = "timer_" .. timer_counter

    -- Cancel self after first callback
    local function callback()
        table.insert(timer_callbacks, {type = "self_cancel", id = timer_id})
        -- Timer would cancel itself here
        return true  -- Signal to cancel
    end

    -- shield.timer_once(100, callback, self)
    return timer_id
end

return M
