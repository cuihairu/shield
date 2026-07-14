-- clock_business.lua
-- Game business logic test service for AD-07 layered clock.
--
-- Covers three common game patterns WITHOUT calling any C++ business APIs
-- directly — all time reads go through os.time / os.date / shield.now,
-- which are the exact surfaces hooked by the business clock (AD-07).
-- Tests advance MockClock from C++ to drive these scenarios.

local M = {}

------------------------------------------------------------------------
-- 1. Cache expiry (os.time) — mirrors shield_database_integration.lua:298
------------------------------------------------------------------------
local cache = {}

function M.set_cache(key, value, ttl_seconds)
    cache[key] = { value = value, expires = os.time() + ttl_seconds }
end

function M.get_cache(key)
    local entry = cache[key]
    if not entry then return nil end
    if entry.expires <= os.time() then
        cache[key] = nil  -- expired — evict
        return nil
    end
    return entry.value
end

function M.cache_size()
    local n = 0
    for _ in pairs(cache) do n = n + 1 end
    return n
end

------------------------------------------------------------------------
-- 2. Daily check-in (os.date) — typical daily-reset game feature
------------------------------------------------------------------------
local last_checkin_date = ""

function M.checkin()
    local today = os.date("%Y-%m-%d")
    if today == last_checkin_date then
        return false, "already checked in today"
    end
    last_checkin_date = today
    return true, today
end

------------------------------------------------------------------------
-- 3. Cooldown (shield.now) — action with time-gated cooldown
------------------------------------------------------------------------
local last_action_ms = 0

function M.do_action(cooldown_ms)
    local now = shield.now()
    if now - last_action_ms < cooldown_ms then
        return false, "on cooldown"
    end
    last_action_ms = now
    return true
end

return M
