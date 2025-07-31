#include <iostream>
#include <lua.hpp>

#include "shield/database/database_actor_service.hpp"

namespace shield::database::lua_bindings {

// 全局数据库服务实例指针
static DatabaseActorService* g_database_service = nullptr;

// =====================================
// Lua绑定辅助函数
// =====================================

void push_query_result(lua_State* L, const QueryResult& result) {
    lua_createtable(L, 0, 5);  // 创建结果表

    // success字段
    lua_pushboolean(L, result.success);
    lua_setfield(L, -2, "success");

    // error字段
    lua_pushstring(L, result.error.c_str());
    lua_setfield(L, -2, "error");

    // affected_rows字段
    lua_pushinteger(L, static_cast<lua_Integer>(result.affected_rows));
    lua_setfield(L, -2, "affected_rows");

    // last_insert_id字段
    lua_pushinteger(L, static_cast<lua_Integer>(result.last_insert_id));
    lua_setfield(L, -2, "last_insert_id");

    // data字段 - 行数组
    lua_createtable(L, static_cast<int>(result.rows.size()), 0);
    for (size_t i = 0; i < result.rows.size(); ++i) {
        lua_createtable(L, 0, static_cast<int>(result.rows[i].size()));

        for (const auto& pair : result.rows[i]) {
            lua_pushstring(L, pair.second.c_str());
            lua_setfield(L, -2, pair.first.c_str());
        }

        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "data");
}

std::vector<std::string> get_string_array_from_lua(lua_State* L, int index) {
    std::vector<std::string> result;

    if (!lua_istable(L, index)) {
        return result;
    }

    size_t len = lua_rawlen(L, index);
    for (size_t i = 1; i <= len; ++i) {
        lua_rawgeti(L, index, static_cast<lua_Integer>(i));
        if (lua_isstring(L, -1)) {
            result.push_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    return result;
}

// =====================================
// Lua C函数实现
// =====================================

// shield.database.register_database(name, config)
int lua_register_database(lua_State* L) {
    if (!g_database_service) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Database service not initialized");
        return 2;
    }

    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // 解析配置表
    DatabaseConfig config;

    lua_getfield(L, 2, "driver");
    if (lua_isstring(L, -1)) config.driver = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "host");
    if (lua_isstring(L, -1)) config.host = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "port");
    if (lua_isinteger(L, -1))
        config.port = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 2, "database");
    if (lua_isstring(L, -1)) config.database = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "username");
    if (lua_isstring(L, -1)) config.username = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "password");
    if (lua_isstring(L, -1)) config.password = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "max_connections");
    if (lua_isinteger(L, -1))
        config.max_connections = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 2, "connection_timeout");
    if (lua_isinteger(L, -1))
        config.connection_timeout = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 2, "auto_reconnect");
    if (lua_isboolean(L, -1)) config.auto_reconnect = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "charset");
    if (lua_isstring(L, -1)) config.charset = lua_tostring(L, -1);
    lua_pop(L, 1);

    // 注册数据库
    bool success = g_database_service->register_database(name, config);
    lua_pushboolean(L, success);

    if (!success) {
        lua_pushstring(L, "Failed to register database");
        return 2;
    }

    return 1;
}

// shield.database.execute_query(database_name, sql, params)
int lua_execute_query(lua_State* L) {
    if (!g_database_service) {
        lua_createtable(L, 0, 2);
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "success");
        lua_pushstring(L, "Database service not initialized");
        lua_setfield(L, -2, "error");
        return 1;
    }

    const char* database_name = luaL_checkstring(L, 1);
    const char* sql = luaL_checkstring(L, 2);

    std::vector<std::string> params;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        params = get_string_array_from_lua(L, 3);
    }

    // 执行查询
    QueryResult result =
        g_database_service->execute_query_sync(database_name, sql, params);

    // 将结果推送到Lua
    push_query_result(L, result);

    return 1;
}

// shield.database.execute_query_async(database_name, sql, params, callback)
int lua_execute_query_async(lua_State* L) {
    if (!g_database_service) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Database service not initialized");
        return 2;
    }

    const char* database_name = luaL_checkstring(L, 1);
    const char* sql = luaL_checkstring(L, 2);

    std::vector<std::string> params;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        params = get_string_array_from_lua(L, 3);
    }

    // 检查回调函数
    int callback_ref = LUA_NOREF;
    if (lua_gettop(L) >= 4 && lua_isfunction(L, 4)) {
        callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // 启动异步查询
    auto future =
        g_database_service->execute_query_async(database_name, sql, params);

    // 这里需要与Actor系统集成，将future结果通过消息传递给回调
    // 简化实现：直接执行同步查询并调用回调
    if (callback_ref != LUA_NOREF) {
        QueryResult result = future.get();

        lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
        push_query_result(L, result);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            std::cerr << "[DatabaseLua] Callback error: " << lua_tostring(L, -1)
                      << std::endl;
            lua_pop(L, 1);
        }

        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    }

    lua_pushboolean(L, true);
    return 1;
}

// shield.database.get_pool_status(database_name)
int lua_get_pool_status(lua_State* L) {
    if (!g_database_service) {
        lua_pushnil(L);
        return 1;
    }

    const char* database_name = luaL_checkstring(L, 1);
    auto status = g_database_service->get_pool_status(database_name);

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, static_cast<lua_Integer>(status.total_connections));
    lua_setfield(L, -2, "total_connections");

    lua_pushinteger(L, static_cast<lua_Integer>(status.active_connections));
    lua_setfield(L, -2, "active_connections");

    lua_pushinteger(L, static_cast<lua_Integer>(status.available_connections));
    lua_setfield(L, -2, "available_connections");

    return 1;
}

// shield.database.get_registered_databases()
int lua_get_registered_databases(lua_State* L) {
    if (!g_database_service) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    auto databases = g_database_service->get_registered_databases();

    lua_createtable(L, static_cast<int>(databases.size()), 0);
    for (size_t i = 0; i < databases.size(); ++i) {
        lua_pushstring(L, databases[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }

    return 1;
}

// shield.database.begin_transaction(database_name)
int lua_begin_transaction(lua_State* L) {
    if (!g_database_service) {
        lua_pushnil(L);
        lua_pushstring(L, "Database service not initialized");
        return 2;
    }

    const char* database_name = luaL_checkstring(L, 1);

    try {
        // 这里需要更复杂的事务管理，简化实现
        lua_createtable(L, 0, 2);
        lua_pushstring(L, database_name);
        lua_setfield(L, -2, "database_name");
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "active");

        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

// =====================================
// 模块注册
// =====================================

static const luaL_Reg database_functions[] = {
    {"register_database", lua_register_database},
    {"execute_query", lua_execute_query},
    {"execute_query_async", lua_execute_query_async},
    {"get_pool_status", lua_get_pool_status},
    {"get_registered_databases", lua_get_registered_databases},
    {"begin_transaction", lua_begin_transaction},
    {NULL, NULL}};

void register_database_service(lua_State* L, DatabaseActorService& service) {
    g_database_service = &service;

    // 创建shield.database模块
    lua_newtable(L);
    luaL_setfuncs(L, database_functions, 0);

    // 注册到全局shield表
    lua_getglobal(L, "shield");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "shield");
        lua_getglobal(L, "shield");
    }

    lua_pushvalue(L, -2);  // 复制database表
    lua_setfield(L, -2, "database");

    lua_pop(L, 2);  // 清理栈

    std::cout << "[DatabaseLua] Registered database functions to Lua"
              << std::endl;
}

// =====================================
// Actor消息处理辅助函数
// =====================================

// 为Lua Actor提供的便捷数据库操作函数
int lua_actor_query_database(lua_State* L) {
    // 这个函数专门为Actor模型设计
    // 执行查询并通过消息系统返回结果

    const char* database_name = luaL_checkstring(L, 1);
    const char* sql = luaL_checkstring(L, 2);
    const char* callback_actor = luaL_checkstring(L, 3);
    const char* query_id = luaL_checkstring(L, 4);

    std::vector<std::string> params;
    if (lua_gettop(L) >= 5 && lua_istable(L, 5)) {
        params = get_string_array_from_lua(L, 5);
    }

    if (!g_database_service) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Database service not initialized");
        return 2;
    }

    // 异步执行查询
    auto future =
        g_database_service->execute_query_async(database_name, sql, params);

    // 这里应该与Actor系统集成，将结果通过消息发送给callback_actor
    // 简化实现：直接执行并模拟消息发送
    std::thread([future = std::move(future),
                 callback_actor = std::string(callback_actor),
                 query_id = std::string(query_id)]() mutable {
        QueryResult result = future.get();

        std::cout << "[DatabaseLua] Query completed for " << callback_actor
                  << " (ID: " << query_id << "), success: " << result.success
                  << std::endl;

        // 这里应该调用Actor消息系统发送结果
        // send_message_to_actor(callback_actor, "database_result", {query_id,
        // result});
    }).detach();

    lua_pushboolean(L, true);
    return 1;
}

// 注册Actor专用的数据库函数
void register_actor_database_functions(lua_State* L) {
    lua_getglobal(L, "shield");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "shield");
        lua_getglobal(L, "shield");
    }

    lua_getfield(L, -1, "database");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, -2, "database");
        lua_getfield(L, -1, "database");
    }

    // 添加Actor专用函数
    lua_pushcfunction(L, lua_actor_query_database);
    lua_setfield(L, -2, "actor_query");

    lua_pop(L, 2);  // 清理栈
}

}  // namespace shield::database::lua_bindings