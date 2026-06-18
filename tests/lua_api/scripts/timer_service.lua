-- Test service for LAPI-007 (Timers and Tasks)
--
-- Drives the timer/fork API surface from on_init so that a test harness can
-- call manager->pump_once() (or rely on the worker) to fire callbacks and
-- then read back recorded state via get_callback_count / get_fork_count.

local M = {}

local timer_events = {}
local fork_events = {}
local last_timer_id = nil

local function record(kind, payload)
    if kind == "timer" then
        table.insert(timer_events, payload)
    elseif kind == "fork" then
        table.insert(fork_events, payload)
    end
end

function M.on_init(args)
    local config = (args and args.config) or {}
    M.test_case = config.test_case or "default"

    if M.test_case == "timer_once" then
        last_timer_id = shield.timer_once(50, function()
            record("timer", {kind = "once", at = shield.now()})
        end)
    elseif M.test_case == "fixed_delay" then
        last_timer_id = shield.timer(40, function()
            record("timer", {kind = "fixed", at = shield.now()})
        end)
    elseif M.test_case == "cancel" then
        last_timer_id = shield.timer_once(200, function()
            record("timer", {kind = "cancelled_should_not_fire"})
        end)
    elseif M.test_case == "error" then
        shield.timer_once(20, function()
            error("timer_callback_error")
        end)
    elseif M.test_case == "fork" then
        shield.fork(function()
            record("fork", {kind = "fork", at = shield.now()})
        end)
    end
end

function M.get_timer_count()
    return #timer_events
end

function M.get_fork_count()
    return #fork_events
end

function M.get_last_timer_id()
    return last_timer_id
end

return M
