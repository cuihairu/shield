-- shield_global P0 test service.
--
-- Entry points take the dispatch ctx first (invoke_coroutine prepend_ctx,
-- same convention as server_service.lua). Scheduler task callbacks record
-- into M.sched_calls so the C++ test can poll asynchronous deliveries.
local M = {}

M.sched_calls = {}
M.init_args = nil

function M.on_init(args)
    M.init_args = args or {}
end

-- ---------------------------------------------------------------------------
-- global data
-- ---------------------------------------------------------------------------

function M.data_roundtrip(ctx)
    local g, err = shield.global()
    if not g then return {ok = false, code = err.code} end
    local results = {ok = true}

    g:set("user:1", {name = "ada", level = 3}, 0)
    local value = g:get("user:1")
    results.get_value = value
    results.missing = g:get("nope")

    g:incr("counter", 1)
    g:incr("counter", 41)
    g:decr("counter", 2)
    results.counter = g:get("counter")

    g:mset({a = "1", b = {x = true}})
    local mget = g:mget("a", "b", "zz")
    results.mget_a = mget[1]
    results.mget_b_x = mget[2] and mget[2].x
    results.mget_missing = mget[3]

    results.deleted = g:delete("user:1")
    results.deleted_again = g:delete("user:1")

    -- local cache: miss -> fill -> hit -> invalidate
    g:set("cached", "v", 0)
    results.cached_1 = g:get_cached("cached", 60000)
    results.cached_2 = g:get_cached("cached", 60000)
    g:invalidate("cached")
    results.cached_3 = g:get_cached("cached", 60000)

    -- incr error on non-numeric payload
    g:set("text", {hello = true}, 0)
    local _, ierr = g:incr("text", 1)
    results.incr_error = ierr and ierr.code

    return results
end

-- ---------------------------------------------------------------------------
-- locks
-- ---------------------------------------------------------------------------

-- Exclusive lock matrix. `preset` only enables the in-matrix compete
-- probe, which is blocked by the `l` hold itself (same owner rules).
function M.lock_matrix(ctx, preset)
    local results = {}
    local l = shield.mutex("test_lock", {ttl = 60000})
    results.try1 = l:try_acquire()
    -- same-object reentry is allowed
    results.try2 = l:try_acquire()
    if preset then
        -- A competing owner (the C++ test) holds it: non-blocking fail.
        results.compete = shield.mutex("test_lock", {ttl = 60000}):try_acquire()
    end
    results.release1 = l:release()
    results.release2 = l:release()
    results.release_unheld = l:release()
    results.owner_info = l:owner() ~= nil
    results.ttl_unheld = l:ttl()
    results.try3 = l:try_acquire()
    results.extend = l:extend(30000)
    results.ttl_held = l:ttl() > 0
    -- with() runs the body and releases; pack the (ok, value) returns
    -- into a table so both survive result serialization.
    results.with = {l:with(function() return "ran" end)}
    results.released_after_with = not l:release()
    -- acquire with timeout succeeds once free
    local l2 = shield.mutex("test_lock2", {retry = 5})
    results.acquire_timeout = l2:acquire(200)
    results.acquire_after_release = (function()
        l2:release()
        local ok = l2:acquire(50)
        l2:release()  -- leave the registry clean for the size check
        return ok
    end)()
    -- spinlock facade shares the same semantics under its own registry
    local s = shield.spinlock("test_lock", {ttl = 60000})
    results.spinlock_free = s:try_acquire()
    s:release()
    local probe = shield.mutex("test_lock", {ttl = 60000})
    results.spinlock_mutex_independent = probe:try_acquire()
    probe:release()
    -- distributed twins ride the same backend in P0
    local d = shield.distributed_mutex("dist_lock", {ttl = 60000})
    results.dist_try = d:try_acquire()
    results.dist_compete = shield.distributed_mutex("dist_lock",
                                                    {ttl = 60000}):try_acquire()
    d:release()
    local drw = shield.distributed_rwlock("dist_rw")
    results.dist_rw_read = drw:read_lock():try_acquire()
    return results
end

-- acquire(timeout) against a lock the C++ test holds for ~120ms.
function M.lock_wait(ctx, hold_ms)
    local l = shield.mutex("wait_lock", {retry = 10})
    local t0 = shield.now()
    local ok = l:acquire((hold_ms or 200) * 3)
    local waited = shield.now() - t0
    return {ok = ok, waited_ms = waited}
end

function M.rwlock_matrix(ctx)
    local results = {}
    local rw = shield.rwlock("rw_test")
    local r1 = rw:read_lock()
    local r2 = rw:read_lock()
    results.read_shared = r1:try_acquire() and r2:try_acquire()
    local w = rw:write_lock()
    results.write_blocked_by_readers = not w:try_acquire()
    results.read_release1 = r1:release()
    results.read_release2 = r2:release()
    results.write_now_free = w:try_acquire()
    results.read_blocked_by_writer = not rw:read_lock():try_acquire()
    results.write_reentrant = w:try_acquire()
    results.write_release1 = w:release()
    results.write_release2 = w:release()
    results.read_after_write = rw:read_lock():try_acquire()
    -- with() on a read guard
    results.read_with = {rw:read_lock():with(function() return 7 end)}
    return results
end

-- ---------------------------------------------------------------------------
-- rank
-- ---------------------------------------------------------------------------

function M.rank_matrix(ctx)
    local results = {}
    local r = shield.rank("board")
    r:update("p1", 1000)
    r:update("p2", 1100)
    r:update("p3", 1000)  -- tie with p1: uid asc -> p1 ranks first
    r:mupdate({p4 = 900})
    results.count = r:count()
    local top = r:top(2)
    results.top1 = top[1].uid
    results.top1_rank = top[1].rank
    results.top1_score = top[1].score
    results.top2 = top[2].uid
    results.position = r:position("p3")
    results.position_missing = r:position("ghost")
    results.score = r:score("p2")
    results.score_missing = r:score("ghost")
    local range = r:range(2, 3)
    results.range1 = range[1].uid
    results.range_size = #range
    local by_score = r:range_by_score(950, 1050)
    results.by_score_size = #by_score
    results.by_score_first = by_score[1].uid
    local around = r:around("p2", 1)
    results.around_target = around.target and around.target.uid
    results.around_above = #around.above
    results.around_below = #around.below
    results.around_below_uid = around.below[1] and around.below[1].uid
    results.removed = r:remove("p4")
    results.removed_again = r:remove("p4")
    results.count_after_remove = r:count()
    r:clear()
    results.count_after_clear = r:count()
    return results
end

-- ---------------------------------------------------------------------------
-- queues
-- ---------------------------------------------------------------------------

function M.queue_matrix(ctx)
    local results = {}
    local q = shield.queue("tasks")
    results.pop_empty = q:pop()
    q:push({job = "a"})
    q:push_batch({{job = "b"}, {job = "c"}})
    results.length = q:length()
    local a = q:pop()
    results.pop_a = a and a.job
    -- bounded wait for a delayed entry (waits through shield.sleep)
    local d = shield.delay_queue("rewards")
    d:push({reward = "late"}, 150)
    d:push_at({reward = "now"}, os.time() - 5)
    results.delay_pending = d:pending()
    results.delay_ready = d:ready()
    -- The already-due entry pops immediately (earliest deadline first);
    -- the delayed entry comes out through the bounded wait.
    local now = d:pop(50)
    results.delay_pop_now = now and now.reward
    local late = d:pop(3000)
    results.delay_pop_late = late and late.reward
    local b = q:pop(100)
    results.pop_b = b and b.job
    q:purge()
    results.length_after_purge = q:length()
    return results
end

function M.reliable_matrix(ctx)
    local results = {}
    local q = shield.reliable_queue("jobs", {max_retries = 2})
    q:push({task = "one"})
    q:push({task = "two"})
    local one, h1 = q:pop()
    results.pop_one = one and one.task
    results.handle_id = h1 and h1.id
    results.ack = h1:ack()
    results.ack_again = h1:ack()
    local two, h2 = q:pop()
    results.nack = h2:nack(50)
    -- after the retry delay the delivery comes back
    local again, h3 = q:pop(3000)
    results.redelivered = again and again.task
    results.nack_dead = h3:nack(0)
    local dead = q:dead_letter()
    results.dead_size = dead:size()
    local msgs = dead:range(0, 10)
    results.dead_msg = msgs[1] and msgs[1].task
    dead:purge()
    results.dead_size_after = dead:size()
    return results
end

-- ---------------------------------------------------------------------------
-- scheduler
-- ---------------------------------------------------------------------------

function M.sched_register(ctx, kind, name, schedule)
    local s = shield.scheduler()
    local fn = function(ctx2)
        table.insert(M.sched_calls, {name = name})
    end
    local ok, err
    if kind == "interval" then
        ok, err = s:interval(name, schedule, fn)
    elseif kind == "once" then
        ok, err = s:once(name, schedule, fn)
    else
        ok, err = s:cron(name, schedule, fn)
    end
    if not ok then
        return {ok = false, code = type(err) == 'table' and err.code
                    or tostring(err)}
    end
    return {ok = true}
end

function M.sched_info(ctx, name)
    local s = shield.scheduler()
    local info = s:get(name)
    if not info then return {exists = false} end
    return {exists = true, status = info.status, type = info.type,
            run_count = info.run_count, next_run = info.next_run}
end

function M.sched_control(ctx, op, name)
    local s = shield.scheduler()
    if op == "pause" then return s:pause(name) end
    if op == "resume" then return s:resume(name) end
    if op == "remove" then return s:remove(name) end
    if op == "trigger" then return s:trigger(name) end
    return false
end

function M.sched_bad_register(ctx, kind, name, schedule, cb_kind)
    local s = shield.scheduler()
    local fn = cb_kind == "none" and "not_a_function" or function() end
    local ok, err
    if kind == "interval" then
        ok, err = s:interval(name, schedule, fn)
    elseif kind == "once" then
        ok, err = s:once(name, schedule, fn)
    else
        ok, err = s:cron(name, schedule, fn)
    end
    return {ok = ok and true or false,
            code = not ok and ((type(err) == 'table' and err.code)
                or tostring(err)) or nil}
end

function M.sched_calls_json(ctx)
    return M.sched_calls
end

function M.sched_count(ctx)
    local impl = rawget(_G, '__shield_global_impl')
    return impl and impl.sched_count() or -1
end

-- rate_limiter: token bucket drain + key isolation + bounded wait +
-- exact sliding window.
function M.rate_limiter_matrix(ctx)
    local results = {}
    -- rate 10/s refills 1 token per 100ms: the post-drain deny below is
    -- stable against sub-millisecond test jitter.
    local l = shield.rate_limiter("api_limit", {rate = 10, burst = 5})
    results.allow1 = l:allow("ip1")
    l:allow("ip1")
    l:allow("ip1")
    l:allow("ip1")
    results.remaining_after4 = l:remaining("ip1")
    results.allow5 = l:allow("ip1")
    results.remaining_after5 = l:remaining("ip1")
    results.allow6 = l:allow("ip1")
    results.other_key = l:allow("ip2")
    local w = shield.rate_limiter("wait_limit", {rate = 1000, burst = 1})
    w:allow("k")
    results.wait_ok = w:wait("k", 500)
    local s = shield.rate_limiter("strict",
                                  {sliding = true, window = 60000,
                                   max_requests = 3})
    s:allow("u")
    s:allow("u")
    results.sliding_remaining = s:remaining("u")
    results.sliding3 = s:allow("u")
    results.sliding4 = s:allow("u")
    results.sliding_fresh = s:remaining("nobody")
    return results
end

-- Stub probe: drives every factory (compiled-out build) and reports the
-- stable error code each one produces.
function M.stub_probe(ctx)
    local codes = {}
    local function probe(name, fn)
        local _, err = fn()
        codes[name] = type(err) == 'table' and err.code or tostring(err)
    end
    probe('global', function() return shield.global() end)
    probe('mutex', function() return shield.mutex("l") end)
    probe('rwlock', function() return shield.rwlock("l") end)
    probe('spinlock', function() return shield.spinlock("l") end)
    probe('distributed_mutex', function()
        return shield.distributed_mutex("l")
    end)
    probe('distributed_rwlock', function()
        return shield.distributed_rwlock("l")
    end)
    probe('rank', function() return shield.rank("b") end)
    probe('queue', function() return shield.queue("q") end)
    probe('delay_queue', function() return shield.delay_queue("q") end)
    probe('reliable_queue', function() return shield.reliable_queue("q") end)
    probe('scheduler', function() return shield.scheduler() end)
    probe('rate_limiter', function() return shield.rate_limiter("r") end)
    return codes
end

return M
