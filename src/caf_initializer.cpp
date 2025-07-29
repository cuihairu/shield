#include "shield/caf_type_ids.hpp"
#include "shield/actor/lua_actor.hpp"
#include "caf/init_global_meta_objects.hpp"

void initialize_caf_types() {
    // 由于我们现在使用字符串消息而不是自定义类型，所以使用核心初始化
    caf::core::init_global_meta_objects();
}