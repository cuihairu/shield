#include "shield/caf_type_ids.hpp"
#include "shield/actor/lua_actor.hpp"
#include "caf/init_global_meta_objects.hpp"

void initialize_caf_types() {
    // Since we are now using string messages instead of custom types, use core initialization
    caf::core::init_global_meta_objects();
}