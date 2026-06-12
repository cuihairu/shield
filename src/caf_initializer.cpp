#include "caf/init_global_meta_objects.hpp"
#include "caf/io/middleman.hpp"

void initialize_caf_types() {
    // Since we are now using string messages instead of custom types, use core
    // initialization
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
}
