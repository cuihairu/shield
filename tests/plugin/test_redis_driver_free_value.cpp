// redis.driver free_value deep-release contract.
//
// free_value must release a value tree at ANY depth. XREADGROUP and other
// raw replies nest arrays five levels deep (streams -> entries -> fields),
// and the historical three-level walk leaked the deep string and array nodes
// on every batch. This test builds a tree shaped exactly like that reply and
// asserts that a single free_value call returns the live-allocation count to
// its baseline.
//
// The driver is linked as an object library directly (no dlopen) so the
// test can interpose malloc/calloc/free and count outstanding live blocks.
// The old implementation leaks: it frees the root array, recurses one level
// for the streams, then bulk-frees the entries array without freeing the
// entry/field nodes inside it, so the delta after free_value stays positive.

#define BOOST_TEST_MODULE shield_redis_driver_free_value

#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"

// -- Interposed allocator wrappers -------------------------------------------
// The driver and this test binary both route malloc/calloc/free through
// these wrappers (linker --wrap). The counter tracks live blocks so the
// test can measure exactly what a free_value call releases.

extern "C" {
void* __real_malloc(size_t size);
void* __real_calloc(size_t n, size_t size);
void __real_free(void* ptr);
}

namespace {
std::size_t g_live = 0;
}

extern "C" void* __wrap_malloc(size_t size) {
    void* p = __real_malloc(size);
    if (p) ++g_live;
    return p;
}

extern "C" void* __wrap_calloc(size_t n, size_t size) {
    void* p = __real_calloc(n, size);
    if (p) ++g_live;
    return p;
}

extern "C" void __wrap_free(void* ptr) {
    if (ptr) --g_live;
    __real_free(ptr);
}

// -- Builders ----------------------------------------------------------------
// Allocate a value and register the block with the live counter implicitly
// through the wrapped calloc/malloc.

namespace {

shield_redis_value_v1* alloc_value() {
    return static_cast<shield_redis_value_v1*>(
        std::calloc(1, sizeof(shield_redis_value_v1)));
}

shield_redis_value_v1* make_string(const char* s) {
    auto* v = alloc_value();
    v->type = SHIELD_REDIS_STRING;
    v->str_len = std::strlen(s);
    auto* buf = static_cast<char*>(std::malloc(v->str_len + 1));
    std::memcpy(buf, s, v->str_len + 1);
    v->str = buf;
    return v;
}

// Build an ARRAY node whose items array is heap-allocated; ownership of the
// child pointers transfers into the array (deep copies, same as
// reply_to_value).
shield_redis_value_v1* make_array(std::size_t count) {
    auto* v = alloc_value();
    v->type = SHIELD_REDIS_ARRAY;
    v->item_count = count;
    if (count == 0) return v;
    v->items = static_cast<shield_redis_value_v1*>(
        std::calloc(count, sizeof(shield_redis_value_v1)));
    return v;
}

void set_item(shield_redis_value_v1* arr, std::size_t i,
              shield_redis_value_v1* child) {
    arr->items[i] = *child;
    std::free(child);  // shallow-copy ownership transfer, like reply_to_value
}

// XREADGROUP-shaped tree:
//   reply        ARRAY[stream]
//   stream       ARRAY[name, entries]
//   entries      ARRAY[entry]
//   entry        ARRAY[id, fields]
//   fields       ARRAY[key, value]
shield_redis_value_v1* build_xreadgroup_tree() {
    auto* reply = make_array(1);

    auto* stream = make_array(2);
    set_item(stream, 0, make_string("mystream"));
    auto* entries = make_array(1);
    auto* entry = make_array(2);
    set_item(entry, 0, make_string("1-0"));
    auto* fields = make_array(4);
    set_item(fields, 0, make_string("payload"));
    set_item(fields, 1, make_string("hello"));
    set_item(fields, 2, make_string("note"));
    set_item(fields, 3, make_string("world"));
    set_item(entry, 1, fields);
    set_item(entries, 0, entry);
    set_item(stream, 1, entries);
    set_item(reply, 0, stream);
    return reply;
}

// Resolve the driver vtable once. The instance is shut down after the copy,
// so the returned function pointer stays valid (it is a static lambda).
const shield_redis_v1* driver_vtable() {
    static const shield_redis_v1* vt = [] {
        const auto* abi = shield_plugin_get_v1();
        BOOST_REQUIRE(abi != nullptr);
        shield_plugin_create_args_v1 args{};
        args.instance_id = "free_value_test";
        args.config_json = "{}";
        shield_error_v1 err{};
        shield_plugin_instance_v1* inst = nullptr;
        BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &err), 0);
        BOOST_REQUIRE(inst != nullptr);
        const auto* vtable = static_cast<const shield_redis_v1*>(
            inst->get_interface(inst, SHIELD_REDIS_V1, &err));
        BOOST_REQUIRE(vtable != nullptr);
        BOOST_REQUIRE(vtable->free_value != nullptr);
        // free_value is a static lambda; it stays valid after the instance
        // is destroyed. Copy the function pointer so the test does not touch
        // the freed instance memory.
        static shield_redis_v1 copy = *vtable;
        inst->shutdown(inst);
        return &copy;
    }();
    return vt;
}

}  // namespace

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

BOOST_AUTO_TEST_CASE(free_value_releases_deep_xreadgroup_tree) {
    const shield_redis_v1* vt = driver_vtable();
    const std::size_t baseline = g_live;

    auto* tree = build_xreadgroup_tree();
    BOOST_CHECK_GT(g_live, baseline);  // the tree owns live blocks

    vt->free_value(tree);

    // After a full deep release every block the tree owned is gone.
    BOOST_CHECK_EQUAL(g_live, baseline);
}

BOOST_AUTO_TEST_CASE(free_value_null_and_empty_are_safe) {
    const shield_redis_v1* vt = driver_vtable();
    const std::size_t baseline = g_live;

    vt->free_value(nullptr);

    // Empty array node: the items array is NULL, nothing to release.
    auto* empty = make_array(0);
    vt->free_value(empty);
    BOOST_CHECK_EQUAL(g_live, baseline);
}

BOOST_AUTO_TEST_CASE(free_value_single_string) {
    const shield_redis_v1* vt = driver_vtable();
    const std::size_t baseline = g_live;

    auto* s = make_string("abc");
    vt->free_value(s);
    BOOST_CHECK_EQUAL(g_live, baseline);
}
