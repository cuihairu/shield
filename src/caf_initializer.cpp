#include "caf/init_global_meta_objects.hpp"
#include "caf/io/middleman.hpp"
#include "shield/core/service_message.hpp"

void initialize_caf_types() {
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
    // Register Shield's custom message types so CAF can pattern-match them
    // in actor behaviors and (later) serialize them for remote transport.
    caf::init_global_meta_objects<caf::id_block::shield_lua>();
}
