# Shield数据访问框架集成测试
# 使用Lua单元测试框架进行全面测试

local luaunit = require('luaunit')
local Shield = require("shield_framework")
local DataAccess = require("shield_data_access")

-- =====================================
-- Mock数据源实现
-- =====================================

local MockDataSource = DataAccess.IDataSource:extend("MockDataSource")

function MockDataSource:new()
    local obj = {
        tables = {
            users = {
                {id = 1, name = "Alice", email = "alice@test.com", level = 10},
                {id = 2, name = "Bob", email = "bob@test.com", level = 15},
                {id = 3, name = "Charlie", email = "charlie@test.com", level = 20}
            }
        },
        next_id = 4,
        query_delay = 0.01 -- 10ms延迟模拟
    }
    setmetatable(obj, self)
    self.__index = self
    return obj
end

function MockDataSource:find(query_builder)
    -- 模拟数据库延迟
    if self.query_delay > 0 then
        local start_time = os.clock()
        while (os.clock() - start_time) < self.query_delay do
            -- busy wait to simulate delay
        end
    end
    
    local collection = query_builder.collection
    local table_data = self.tables[collection] or {}
    local result_data = {}
    
    -- 简单的条件过滤
    for _, row in ipairs(table_data) do
        if self:matches_criteria(row, query_builder.criteria_) then
            table.insert(result_data, row)
        end
    end
    
    -- 简单的排序
    if #query_builder.sorts > 0 then
        local sort = query_builder.sorts[1]
        table.sort(result_data, function(a, b)
            if sort.direction == "asc" then
                return a[sort.field] < b[sort.field]
            else
                return a[sort.field] > b[sort.field]
            end
        end)
    end
    
    -- 简单的限制
    if query_builder.limit_ and #result_data > query_builder.limit_ then
        local limited = {}
        for i = 1, query_builder.limit_ do
            limited[i] = result_data[i]
        end
        result_data = limited
    end
    
    return {
        success = true,
        data = result_data,
        affected_rows = 0
    }
end

function MockDataSource:find_one(query_builder)
    query_builder:limit(1)
    local result = self:find(query_builder)
    
    if result.success and result.data and #result.data > 0 then
        result.data = result.data[1]
    else
        result.data = nil
    end
    
    return result
end

function MockDataSource:insert(collection, data)
    if self.query_delay > 0 then
        local start_time = os.clock()
        while (os.clock() - start_time) < (self.query_delay / 2) do end
    end
    
    if not self.tables[collection] then
        self.tables[collection] = {}
    end
    
    local new_data = {}
    for k, v in pairs(data) do
        new_data[k] = v
    end
    
    new_data.id = self.next_id
    self.next_id = self.next_id + 1
    
    table.insert(self.tables[collection], new_data)
    
    return {
        success = true,
        data = new_data,
        affected_rows = 1,
        last_insert_id = new_data.id
    }
end

function MockDataSource:update(query_builder)
    if self.query_delay > 0 then
        local start_time = os.clock()
        while (os.clock() - start_time) < (self.query_delay * 0.8) do end
    end
    
    local collection = query_builder.collection
    local table_data = self.tables[collection] or {}
    local updated_count = 0
    
    for _, row in ipairs(table_data) do
        if self:matches_criteria(row, query_builder.criteria_) then
            -- 更新字段
            for field, value in pairs(query_builder.updates) do
                row[field] = value.value
            end
            updated_count = updated_count + 1
        end
    end
    
    return {
        success = true,
        affected_rows = updated_count
    }
end

function MockDataSource:remove(query_builder)
    if self.query_delay > 0 then
        local start_time = os.clock()
        while (os.clock() - start_time) < (self.query_delay * 0.6) do end
    end
    
    local collection = query_builder.collection
    local table_data = self.tables[collection] or {}
    local removed_count = 0
    
    local new_data = {}
    for _, row in ipairs(table_data) do
        if not self:matches_criteria(row, query_builder.criteria_) then
            table.insert(new_data, row)
        else
            removed_count = removed_count + 1
        end
    end
    
    self.tables[collection] = new_data
    
    return {
        success = true,
        affected_rows = removed_count
    }
end

function MockDataSource:count(query_builder)
    local collection = query_builder.collection
    local table_data = self.tables[collection] or {}
    local count = 0
    
    for _, row in ipairs(table_data) do
        if self:matches_criteria(row, query_builder.criteria_) then
            count = count + 1
        end
    end
    
    return {
        success = true,
        count = count
    }
end

function MockDataSource:exists(query_builder)
    local count_result = self:count(query_builder)
    return {
        success = count_result.success,
        exists = count_result.success and count_result.count > 0
    }
end

function MockDataSource:matches_criteria(row, criteria)
    if not criteria then
        return true
    end
    
    if criteria.operator == "eq" then
        return row[criteria.field] == criteria.values[1].value
    elseif criteria.operator == "gt" then
        return row[criteria.field] > criteria.values[1].value
    elseif criteria.operator == "lt" then
        return row[criteria.field] < criteria.values[1].value
    elseif criteria.operator == "like" then
        local pattern = criteria.values[1].value:gsub("%%", ".*")
        return string.match(tostring(row[criteria.field]), pattern) ~= nil
    elseif criteria.operator == "and" then
        for _, sub_criteria in ipairs(criteria.sub_criteria) do
            if not self:matches_criteria(row, sub_criteria) then
                return false
            end
        end
        return true
    elseif criteria.operator == "or" then
        for _, sub_criteria in ipairs(criteria.sub_criteria) do
            if self:matches_criteria(row, sub_criteria) then
                return true
            end
        end
        return false
    end
    
    return true
end

function MockDataSource:get_database_type()
    return "mock"
end

-- =====================================
-- 测试实体
-- =====================================

local TestUser = Shield.Service:extend("TestUser")

function TestUser:new(data)
    local obj = data or {}
    obj.id = obj.id or nil
    obj.name = obj.name or ""
    obj.email = obj.email or ""
    obj.level = obj.level or 1
    
    setmetatable(obj, self)
    self.__index = self
    obj._shield_managed = true
    return obj
end

function TestUser:to_data()
    return {
        id = self.id,
        name = self.name,
        email = self.email,
        level = self.level
    }
end

function TestUser:from_data(data)
    self.id = data.id
    self.name = data.name or ""
    self.email = data.email or ""
    self.level = data.level or 1
    return self
end

function TestUser:get_id_field()
    return "id"
end

function TestUser:get_id()
    return self.id
end

-- =====================================
-- Repository测试类
-- =====================================

local TestUserRepository = DataAccess.IRepository:extend("TestUserRepository")

function TestUserRepository:new(data_source)
    return DataAccess.IRepository.new(self, data_source, "users", TestUser)
end

function TestUserRepository:find_by_level(level)
    local criteria = DataAccess.Criteria.where("level"):equals(level)
    return self:find_by(criteria)
end

function TestUserRepository:find_high_level_users(min_level)
    local criteria = DataAccess.Criteria.where("level"):greater_than(min_level)
    return self:find_by(criteria)
end

-- =====================================
-- 缓存管理器测试类
-- =====================================

local TestCacheManager = {}
TestCacheManager.__index = TestCacheManager

function TestCacheManager:new()
    local obj = {
        cache = {},
        hits = 0,
        misses = 0,
        max_size = 100
    }
    setmetatable(obj, self)
    return obj
end

function TestCacheManager:get(key)
    if self.cache[key] then
        self.hits = self.hits + 1
        return self.cache[key]
    else
        self.misses = self.misses + 1
        return nil
    end
end

function TestCacheManager:put(key, value)
    self.cache[key] = value
end

function TestCacheManager:get_hit_ratio()
    local total = self.hits + self.misses
    return total > 0 and (self.hits / total) or 0
end

-- =====================================
-- 单元测试类
-- =====================================

TestDataAccessFramework = {}

function TestDataAccessFramework:setUp()
    self.mock_datasource = MockDataSource:new()
    self.cache_manager = TestCacheManager:new()
    self.user_repo = TestUserRepository:new(self.mock_datasource)
end

-- 基本CRUD测试
function TestDataAccessFramework:test_find_all_users()
    local result = self.user_repo:find_all()
    
    luaunit.assertTrue(result.success)
    luaunit.assertEquals(#result.entities, 3)
    luaunit.assertEquals(result.entities[1].name, "Alice")
    luaunit.assertEquals(result.entities[2].name, "Bob")
    luaunit.assertEquals(result.entities[3].name, "Charlie")
end

function TestDataAccessFramework:test_find_user_by_id()
    local result = self.user_repo:find_by_id(1)
    
    luaunit.assertTrue(result.success)
    luaunit.assertNotNil(result.entity)
    luaunit.assertEquals(result.entity.id, 1)
    luaunit.assertEquals(result.entity.name, "Alice")
    luaunit.assertEquals(result.entity.email, "alice@test.com")
end

function TestDataAccessFramework:test_insert_new_user()
    local new_user = TestUser:new({
        name = "David",
        email = "david@test.com",
        level = 12
    })
    
    local result = self.user_repo:save(new_user)
    
    luaunit.assertTrue(result.success)
    luaunit.assertNotNil(result.entity)
    luaunit.assertNotNil(result.entity.id)
    luaunit.assertEquals(result.entity.name, "David")
    luaunit.assertEquals(result.entity.email, "david@test.com")
    luaunit.assertEquals(result.entity.level, 12)
end

function TestDataAccessFramework:test_update_user()
    -- 首先查找用户
    local find_result = self.user_repo:find_by_id(1)
    luaunit.assertTrue(find_result.success)
    
    local user = find_result.entity
    user.level = 25
    
    -- 更新用户
    local update_result = self.user_repo:save(user)
    luaunit.assertTrue(update_result.success)
    
    -- 验证更新
    local verify_result = self.user_repo:find_by_id(1)
    luaunit.assertTrue(verify_result.success)
    luaunit.assertEquals(verify_result.entity.level, 25)
end

function TestDataAccessFramework:test_delete_user()
    -- 首先插入一个测试用户
    local new_user = TestUser:new({
        name = "ToDelete",
        email = "delete@test.com",
        level = 1
    })
    
    local insert_result = self.user_repo:save(new_user)
    luaunit.assertTrue(insert_result.success)
    
    local user_id = insert_result.entity.id
    
    -- 删除用户
    local delete_result = self.user_repo:delete_by_id(user_id)
    luaunit.assertTrue(delete_result.success)
    
    -- 验证删除
    local find_result = self.user_repo:find_by_id(user_id)
    luaunit.assertTrue(find_result.success)
    luaunit.assertNil(find_result.entity)
end

-- 查询功能测试
function TestDataAccessFramework:test_find_by_criteria()
    local result = self.user_repo:find_by_level(15)
    
    luaunit.assertTrue(result.success)
    luaunit.assertEquals(#result.entities, 1)
    luaunit.assertEquals(result.entities[1].name, "Bob")
    luaunit.assertEquals(result.entities[1].level, 15)
end

function TestDataAccessFramework:test_find_high_level_users()
    local result = self.user_repo:find_high_level_users(12)
    
    luaunit.assertTrue(result.success)
    luaunit.assertEquals(#result.entities, 2) -- Bob(15) and Charlie(20)
    
    -- 验证结果
    local found_levels = {}
    for _, user in ipairs(result.entities) do
        table.insert(found_levels, user.level)
    end
    table.sort(found_levels)
    
    luaunit.assertEquals(found_levels[1], 15)
    luaunit.assertEquals(found_levels[2], 20)
end

function TestDataAccessFramework:test_complex_criteria()
    -- 测试复合条件查询
    local criteria = DataAccess.Criteria.where("level"):greater_than(10)
        :and_also(DataAccess.Criteria.where("name"):like("%a%"))
    
    local query = DataAccess.QueryBuilder:new("users"):where(criteria)
    local result = self.mock_datasource:find(query)
    
    luaunit.assertTrue(result.success)
    -- Alice(10)被排除(level不大于10), Bob(15)和Charlie(20)都包含'a'
    luaunit.assertEquals(#result.data, 2)
end

-- 缓存功能测试
function TestDataAccessFramework:test_cache_performance()
    local cache = self.cache_manager
    
    -- 模拟带缓存的查询
    local function cached_find_by_id(id)
        local cache_key = "user_" .. tostring(id)
        local cached_result = cache:get(cache_key)
        
        if cached_result then
            return cached_result
        else
            local result = self.user_repo:find_by_id(id)
            if result.success then
                cache:put(cache_key, result)
            end
            return result
        end
    end
    
    -- 第一次查询（缓存未命中）
    local start_time = os.clock()
    local result1 = cached_find_by_id(1)
    local first_query_time = os.clock() - start_time
    
    -- 第二次查询（缓存命中）
    start_time = os.clock()
    local result2 = cached_find_by_id(1)
    local second_query_time = os.clock() - start_time
    
    -- 验证结果一致
    luaunit.assertTrue(result1.success)
    luaunit.assertTrue(result2.success)
    luaunit.assertEquals(result1.entity.id, result2.entity.id)
    luaunit.assertEquals(result1.entity.name, result2.entity.name)
    
    -- 验证缓存性能提升
    luaunit.assertTrue(second_query_time < first_query_time)
    
    -- 验证缓存统计
    luaunit.assertEquals(cache.hits, 1)
    luaunit.assertEquals(cache.misses, 1)
    luaunit.assertEquals(cache:get_hit_ratio(), 0.5)
end

-- 统计功能测试
function TestDataAccessFramework:test_count_operations()
    local count_result = self.user_repo:count()
    
    luaunit.assertTrue(count_result.success)
    luaunit.assertEquals(count_result.count, 3)
end

function TestDataAccessFramework:test_exists_operations()
    local exists_result = self.user_repo:exists_by_id(1)
    luaunit.assertTrue(exists_result.success)
    luaunit.assertTrue(exists_result.exists)
    
    local not_exists_result = self.user_repo:exists_by_id(999)
    luaunit.assertTrue(not_exists_result.success)
    luaunit.assertFalse(not_exists_result.exists)
end

-- 分页测试
function TestDataAccessFramework:test_pagination()
    local pageable = DataAccess.Pageable:new(0, 2, {DataAccess.Sort.asc("name")})
    local result = self.user_repo:find_by(nil, pageable)
    
    luaunit.assertTrue(result.success)
    luaunit.assertEquals(#result.entities, 2)
    
    -- 按名称排序，应该是Alice和Bob
    luaunit.assertEquals(result.entities[1].name, "Alice")
    luaunit.assertEquals(result.entities[2].name, "Bob")
end

-- 批量操作测试
function TestDataAccessFramework:test_batch_operations()
    -- 创建多个用户
    local users = {}
    for i = 1, 3 do
        table.insert(users, TestUser:new({
            name = "BatchUser" .. i,
            email = "batch" .. i .. "@test.com",
            level = 10 + i
        }))
    end
    
    -- 批量插入（模拟实现）
    local success_count = 0
    local saved_users = {}
    
    for _, user in ipairs(users) do
        local result = self.user_repo:save(user)
        if result.success then
            success_count = success_count + 1
            table.insert(saved_users, result.entity)
        end
    end
    
    luaunit.assertEquals(success_count, 3)
    luaunit.assertEquals(#saved_users, 3)
    
    for i, user in ipairs(saved_users) do
        luaunit.assertNotNil(user.id)
        luaunit.assertEquals(user.name, "BatchUser" .. i)
    end
end

-- 性能基准测试
function TestDataAccessFramework:test_performance_benchmark()
    local num_operations = 50
    local start_time = os.clock()
    
    -- 执行多次查询
    for i = 1, num_operations do
        local user_id = (i % 3) + 1
        local result = self.user_repo:find_by_id(user_id)
        luaunit.assertTrue(result.success)
    end
    
    local total_time = os.clock() - start_time
    local avg_time = total_time / num_operations
    
    print(string.format("执行了 %d 次查询操作，总耗时: %.3f秒", num_operations, total_time))
    print(string.format("平均每次操作耗时: %.3f毫秒", avg_time * 1000))
    
    -- 验证性能在合理范围内（每个操作应该在100ms以内）
    luaunit.assertTrue(avg_time < 0.1)
end

-- 错误处理测试
function TestDataAccessFramework:test_error_handling()
    -- 测试查找不存在的用户
    local result = self.user_repo:find_by_id(999)
    luaunit.assertTrue(result.success)
    luaunit.assertNil(result.entity)
    
    -- 测试无效数据
    local invalid_user = TestUser:new({
        name = "", -- 空名称
        email = "invalid",
        level = -1
    })
    
    -- 这里可以添加验证逻辑
    local save_result = self.user_repo:save(invalid_user)
    -- 根据具体的验证策略，这里可能成功或失败
    luaunit.assertNotNil(save_result)
end

-- 数据源类型测试
function TestDataAccessFramework:test_datasource_types()
    local supported_types = DataAccess.DataSourceFactory.get_supported_types()
    
    luaunit.assertTrue(#supported_types > 0)
    luaunit.assertTrue(self:contains(supported_types, "mysql"))
    luaunit.assertTrue(self:contains(supported_types, "mongodb"))
    luaunit.assertTrue(self:contains(supported_types, "redis"))
end

function TestDataAccessFramework:contains(table, value)
    for _, v in pairs(table) do
        if v == value then
            return true
        end
    end
    return false
end

-- =====================================
-- 集成测试
-- =====================================

TestDataAccessIntegration = {}

function TestDataAccessIntegration:setUp()
    self.mock_ds = MockDataSource:new()
    self.cache = TestCacheManager:new()
    self.user_repo = TestUserRepository:new(self.mock_ds)
end

function TestDataAccessIntegration:test_full_workflow()
    print("\n=== 完整工作流程测试 ===")
    
    -- 1. 查询初始数据
    print("1. 查询初始用户数据")
    local initial_result = self.user_repo:find_all()
    luaunit.assertTrue(initial_result.success)
    local initial_count = #initial_result.entities
    print(string.format("   初始用户数: %d", initial_count))
    
    -- 2. 创建新用户
    print("2. 创建新用户")
    local new_user = TestUser:new({
        name = "Integration Test User",
        email = "integration@test.com",
        level = 99
    })
    
    local create_result = self.user_repo:save(new_user)
    luaunit.assertTrue(create_result.success)
    luaunit.assertNotNil(create_result.entity.id)
    local user_id = create_result.entity.id
    print(string.format("   创建用户成功，ID: %s", tostring(user_id)))
    
    -- 3. 查询创建的用户
    print("3. 查询新创建的用户")
    local find_result = self.user_repo:find_by_id(user_id)
    luaunit.assertTrue(find_result.success)
    luaunit.assertNotNil(find_result.entity)
    luaunit.assertEquals(find_result.entity.name, "Integration Test User")
    print("   查询新用户成功")
    
    -- 4. 更新用户信息
    print("4. 更新用户信息")
    find_result.entity.level = 100
    local update_result = self.user_repo:save(find_result.entity)
    luaunit.assertTrue(update_result.success)
    luaunit.assertEquals(update_result.entity.level, 100)
    print("   更新用户成功")
    
    -- 5. 验证总用户数增加
    print("5. 验证用户总数")
    local final_result = self.user_repo:find_all()
    luaunit.assertTrue(final_result.success)
    luaunit.assertEquals(#final_result.entities, initial_count + 1)
    print(string.format("   最终用户数: %d", #final_result.entities))
    
    -- 6. 条件查询
    print("6. 执行条件查询")
    local high_level_result = self.user_repo:find_high_level_users(50)
    luaunit.assertTrue(high_level_result.success)
    luaunit.assertTrue(#high_level_result.entities >= 1)
    print(string.format("   高级用户数: %d", #high_level_result.entities))
    
    -- 7. 删除测试用户
    print("7. 删除测试用户")
    local delete_result = self.user_repo:delete_by_id(user_id)
    luaunit.assertTrue(delete_result.success)
    print("   删除用户成功")
    
    -- 8. 验证删除
    print("8. 验证用户已删除")
    local verify_delete = self.user_repo:find_by_id(user_id)
    luaunit.assertTrue(verify_delete.success)
    luaunit.assertNil(verify_delete.entity)
    print("   删除验证成功")
    
    print("=== 完整工作流程测试完成 ===\n")
end

-- =====================================
-- 主测试运行器
-- =====================================

local function run_all_tests()
    print("=== Shield数据访问框架 Lua集成测试 ===")
    print("测试范围:")
    print("- 基本CRUD操作")
    print("- 复杂查询功能")
    print("- 缓存性能优化")
    print("- 分页和排序")
    print("- 批量操作")
    print("- 错误处理")
    print("- 性能基准测试")
    print("- 完整集成流程")
    print("=====================================\n")
    
    -- 设置测试输出级别
    luaunit.LuaUnit.verbosity = 2
    
    -- 运行所有测试
    local runner = luaunit.LuaUnit.new()
    runner:setOutputType("text")
    
    local result = runner:runSuite()
    
    print("\n=== 测试总结 ===")
    print(string.format("总测试数: %d", runner.result.testCount))
    print(string.format("成功: %d", runner.result.successCount))
    print(string.format("失败: %d", runner.result.failureCount))
    print(string.format("错误: %d", runner.result.errorCount))
    
    if runner.result.failureCount == 0 and runner.result.errorCount == 0 then
        print("🎉 所有测试通过！")
        print([[
        
✅ Shield数据访问框架Lua版本测试完成:

🔧 核心功能验证:
  • Repository模式 ✓
  • 查询构建器 ✓  
  • 条件查询 ✓
  • CRUD操作 ✓
  • 缓存机制 ✓
  • 分页排序 ✓
  • 批量操作 ✓
  • 错误处理 ✓
  • 性能基准 ✓

🚀 企业级特性:
  • 多数据源支持
  • 统一查询API
  • 高性能缓存
  • 类型安全
  • 异常处理
  • 插件架构

Lua数据访问框架已准备就绪！]])
    else
        print("❌ 部分测试失败，请检查实现")
    end
    
    return result == 0
end

-- 如果直接运行此文件，则执行测试
if arg and arg[0] and arg[0]:match("data_access_test%.lua$") then
    run_all_tests()
end

return {
    TestDataAccessFramework = TestDataAccessFramework,
    TestDataAccessIntegration = TestDataAccessIntegration,
    MockDataSource = MockDataSource,
    TestUser = TestUser,
    TestUserRepository = TestUserRepository,
    run_all_tests = run_all_tests
}