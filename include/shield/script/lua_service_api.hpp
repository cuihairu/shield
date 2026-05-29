// [CORE]
#pragma once

#include <sol/sol.hpp>
#include <string>

namespace shield::service {
class ServiceContext;
}

namespace shield::script {

// 将 Skynet 风格的 service API 暴露为 Lua 全局表 shield.*
// 在 LuaActor::register_cpp_functions() 中调用 register_api().
class LuaServiceApi {
public:
    // 注册 shield.send / shield.call / shield.query / shield.timeout /
    //        shield.name / shield.uniqueservice / shield.list_services /
    //        shield.self / shield.node_id 到指定 lua state.
    static void register_api(sol::state& lua);
};

}  // namespace shield::script
