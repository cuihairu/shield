local M = {}

local function contains(list, value)
    for _, item in ipairs(list) do
        if item == value then
            return true
        end
    end
    return false
end

function M.on_init(args)
    local queried, query_err = shield.query(args.name)
    assert(queried == nil and query_err.code == "service_not_found", "init name must not be published yet")

    local child, spawn_err = shield.spawn("registry_child", {
        name = "registry_child.1",
    })
    assert(child, spawn_err and spawn_err.message or "spawn child failed")

    local queried_child = shield.query("registry_child.1")
    assert(queried_child == child, "default child name should query")

    local ok_alias, alias_err = shield.call(child, "publish_alias", "registry.alias")
    assert(ok_alias == true, alias_err and alias_err.message or "alias publish failed")

    local alias = shield.query("registry.alias")
    assert(alias == child, "alias should resolve to child")

    local names = shield.names()
    assert(contains(names, "registry_child.1"), "names should include default child name")
    assert(contains(names, "registry.alias"), "names should include alias")

    local ok_ping, reply = shield.call("registry.alias", "ping", "hello")
    assert(ok_ping == true and reply == "hello:registry_root", "call through alias failed")

    local ok_duplicate, duplicate_err = shield.register("registry.alias")
    assert(ok_duplicate == false and duplicate_err.code == "register_failed", "duplicate register should fail")

    local ok_unpublish = shield.call(child, "unpublish_alias", "registry.alias")
    assert(ok_unpublish == true, "alias unpublish failed")
    local missing = shield.query("registry.alias")
    assert(missing == nil, "alias should be gone after unregister")

    shield.exit("registry_smoke_done")
end

function M.on_exit(reason)
    shield.log.info(reason)
end

return M
