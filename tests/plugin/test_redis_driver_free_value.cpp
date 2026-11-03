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
#include <string>

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
bool g_fail_next_calloc = false;
}  // namespace

extern "C" void* __wrap_malloc(size_t size) {
    void* p = __real_malloc(size);
    if (p) ++g_live;
    return p;
}

extern "C" void* __wrap_calloc(size_t n, size_t size) {
    if (g_fail_next_calloc) {
        g_fail_next_calloc = false;
        return nullptr;
    }
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
const shield_redis_v2* driver_vtable() {
    static const shield_redis_v2* vt = [] {
        const auto* abi = shield_plugin_get_v1();
        BOOST_REQUIRE(abi != nullptr);
        shield_plugin_create_args_v1 args{};
        args.instance_id = "free_value_test";
        args.config_json = "{}";
        shield_error_v1 err{};
        shield_plugin_instance_v1* inst = nullptr;
        BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &err), 0);
        BOOST_REQUIRE(inst != nullptr);
        const auto* vtable = static_cast<const shield_redis_v2*>(
            inst->get_interface(inst, SHIELD_REDIS_V2, &err));
        BOOST_REQUIRE(vtable != nullptr);
        BOOST_REQUIRE(vtable->free_value != nullptr);
        // free_value is a static lambda; it stays valid after the instance
        // is destroyed. Copy the function pointer so the test does not touch
        // the freed instance memory.
        static shield_redis_v2 copy = *vtable;
        inst->shutdown(inst);
        return &copy;
    }();
    return vt;
}

}  // namespace

extern "C" const shield_plugin_abi_v1* shield_plugin_get_v1(void);

BOOST_AUTO_TEST_CASE(free_value_releases_deep_xreadgroup_tree) {
    const shield_redis_v2* vt = driver_vtable();
    const std::size_t baseline = g_live;

    auto* tree = build_xreadgroup_tree();
    BOOST_CHECK_GT(g_live, baseline);  // the tree owns live blocks

    vt->free_value(tree);

    // After a full deep release every block the tree owned is gone.
    BOOST_CHECK_EQUAL(g_live, baseline);
}

BOOST_AUTO_TEST_CASE(free_value_null_and_empty_are_safe) {
    const shield_redis_v2* vt = driver_vtable();
    const std::size_t baseline = g_live;

    vt->free_value(nullptr);

    // Empty array node: the items array is NULL, nothing to release.
    auto* empty = make_array(0);
    vt->free_value(empty);
    BOOST_CHECK_EQUAL(g_live, baseline);
}

BOOST_AUTO_TEST_CASE(free_value_single_string) {
    const shield_redis_v2* vt = driver_vtable();
    const std::size_t baseline = g_live;

    auto* s = make_string("abc");
    vt->free_value(s);
    BOOST_CHECK_EQUAL(g_live, baseline);
}

// pipeline() returns shield_redis_value_v1** (a heap-allocated array of
// independently allocated value nodes). The ABI contract is:
//   free_value(out_array[i]) for each element, then free(out_array).
// This test pins that exact ownership shape so a flat inline-element array
// cannot regress the ABI and trigger invalid frees / heap corruption.
BOOST_AUTO_TEST_CASE(free_value_pipeline_contract) {
    const shield_redis_v2* vt = driver_vtable();
    const std::size_t baseline = g_live;
    const std::size_t count = 3;

    // Simulate the exact allocation shape produced by pipeline(): an array
    // of pointers, each pointing to an independently allocated value node.
    auto** out_array = static_cast<shield_redis_value_v1**>(
        std::calloc(count, sizeof(shield_redis_value_v1*)));
    out_array[0] = make_string("PING");
    out_array[1] = make_array(0);  // empty reply
    auto* nested = make_array(1);
    set_item(nested, 0, make_string("nested"));
    out_array[2] = nested;

    // Exercise the documented pipeline free sequence exactly.
    for (std::size_t i = 0; i < count; ++i) {
        vt->free_value(out_array[i]);
    }
    std::free(out_array);

    BOOST_CHECK_EQUAL(g_live, baseline);
}

// -- Real pipeline ABI integration -----------------------------------------
// Spawns a local redis-server (when available, POSIX only) and exercises the
// actual pipeline() implementation. This pins the end-to-end ownership
// contract: each out_array[i] is independently allocated, freed by
// free_value, then out_array itself is freed. The old flat-array bug caused
// invalid free + double free here.
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct redis_server {
    int port = 0;
    ::pid_t pid = -1;
    ~redis_server() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

int probe_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_accepts(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

std::unique_ptr<redis_server> spawn_redis_server() {
    const int port = probe_free_port();
    if (port <= 0) return nullptr;
    const ::pid_t pid = ::fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        const auto port_str = std::to_string(port);
        ::execlp("redis-server", "redis-server", "--port", port_str.c_str(),
                 "--save", "", "--appendonly", "no", nullptr);
        ::_exit(127);
    }
    for (int i = 0; i < 100; ++i) {
        if (tcp_accepts(port)) {
            auto srv = std::make_unique<redis_server>();
            srv->port = port;
            srv->pid = pid;
            return srv;
        }
        ::usleep(50000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return nullptr;
}

}  // namespace

BOOST_AUTO_TEST_CASE(pipeline_abi_real_server_ownership_contract) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }

    const auto* abi = shield_plugin_get_v1();
    BOOST_REQUIRE(abi != nullptr);
    shield_plugin_create_args_v1 args{};
    args.instance_id = "free_value_pipeline_real";
    const std::string config_json =
        R"({"host":"127.0.0.1","port":)" + std::to_string(srv->port) + "}";
    args.config_json = config_json.c_str();
    shield_error_v1 err{};
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_REQUIRE_EQUAL(abi->create(&args, &inst, &err), 0);
    BOOST_REQUIRE(inst != nullptr);
    BOOST_REQUIRE_EQUAL(inst->start(inst, &err), 0);

    // A v2 driver must NOT hand out the frozen v1 identifier: an old
    // consumer compiled against the flat-array pipeline would free_value()
    // each pointer-sized slot as a value node and corrupt the heap. Asking
    // for v1 returns NULL instead of a silently-incompatible vtable.
    {
        shield_error_v1 legacy_err{};
        const void* legacy =
            inst->get_interface(inst, SHIELD_REDIS_V1, &legacy_err);
        BOOST_CHECK(legacy == nullptr);
    }

    const auto* vt = static_cast<const shield_redis_v2*>(
        inst->get_interface(inst, SHIELD_REDIS_V2, &err));
    BOOST_REQUIRE(vt != nullptr);
    BOOST_REQUIRE(vt->pipeline != nullptr);
    BOOST_REQUIRE(vt->free_value != nullptr);

    void* handle = vt->connect(vt, nullptr, nullptr, 0);
    BOOST_REQUIRE(handle != nullptr);

    const std::string k1 = "shield:test:p1:" + std::to_string(::getpid());
    const std::string v1 = "value1";
    const std::string k2 = "shield:test:p2:" + std::to_string(::getpid());
    const std::string v2 = "value2";
    const std::string set1[] = {"SET", k1, v1};
    const std::string set2[] = {"SET", k2, v2};
    const std::string get1[] = {"GET", k1};
    const std::string get2[] = {"GET", k2};
    const shield_redis_arg_v1 set1_args[] = {
        {set1[0].data(), set1[0].size()},
        {set1[1].data(), set1[1].size()},
        {set1[2].data(), set1[2].size()},
    };
    const shield_redis_arg_v1 set2_args[] = {
        {set2[0].data(), set2[0].size()},
        {set2[1].data(), set2[1].size()},
        {set2[2].data(), set2[2].size()},
    };
    const shield_redis_arg_v1 get1_args[] = {
        {get1[0].data(), get1[0].size()},
        {get1[1].data(), get1[1].size()},
    };
    const shield_redis_arg_v1 get2_args[] = {
        {get2[0].data(), get2[0].size()},
        {get2[1].data(), get2[1].size()},
    };
    const shield_redis_command_v1 cmds[] = {
        {set1_args, 3},
        {set2_args, 3},
        {get1_args, 2},
        {get2_args, 2},
    };

    shield_redis_value_v1** out_array = nullptr;
    uint64_t out_count = 0;

    // A zero-command pipeline is a valid no-op: it succeeds with zero replies.
    // calloc(0, size) is implementation-defined (null or a unique pointer), so
    // the caller must only rely on out_count == 0 and never dereference the
    // array. This pins that the driver never touches out_array when empty.
    {
        shield_error_v1 empty_err{};
        shield_redis_value_v1** empty_array = nullptr;
        uint64_t empty_count = 0;
        const int empty_rc = vt->pipeline(handle, cmds, 0, &empty_array,
                                          &empty_count, &empty_err);
        BOOST_CHECK_EQUAL(empty_rc, 0);
        BOOST_CHECK_EQUAL(empty_count, 0u);
        // Release whatever calloc(0) returned (null is a safe free; a non-null
        // pointer from calloc(0) must also be freed).
        std::free(empty_array);
    }

    // Allocation failure is an ABI error, never a null dereference on return.
    {
        g_fail_next_calloc = true;
        shield_error_v1 alloc_err{};
        shield_redis_value_v1** alloc_array = nullptr;
        uint64_t alloc_count = 0;
        const int alloc_rc = vt->pipeline(handle, cmds, 4, &alloc_array,
                                          &alloc_count, &alloc_err);
        BOOST_CHECK_EQUAL(alloc_rc, -1);
        BOOST_CHECK(alloc_array == nullptr);
        BOOST_CHECK_EQUAL(alloc_count, 0u);
        BOOST_CHECK_EQUAL(std::string(alloc_err.code ? alloc_err.code : ""),
                          "redis.pipeline.failed");
    }

    const int rc = vt->pipeline(handle, cmds, 4, &out_array, &out_count, &err);
    BOOST_REQUIRE_EQUAL(rc, 0);
    BOOST_REQUIRE(out_array != nullptr);
    BOOST_REQUIRE_EQUAL(out_count, 4u);

    // Every element is an independently allocated value node.
    for (uint64_t i = 0; i < out_count; ++i) {
        BOOST_REQUIRE(out_array[i] != nullptr);
    }
    BOOST_REQUIRE(out_array[2] != nullptr);
    BOOST_CHECK_EQUAL(out_array[2]->type, SHIELD_REDIS_STRING);
    BOOST_CHECK_EQUAL(std::string(out_array[2]->str, out_array[2]->str_len),
                      v1);
    BOOST_REQUIRE(out_array[3] != nullptr);
    BOOST_CHECK_EQUAL(out_array[3]->type, SHIELD_REDIS_STRING);
    BOOST_CHECK_EQUAL(std::string(out_array[3]->str, out_array[3]->str_len),
                      v2);

    const std::size_t baseline = g_live;
    // Exercise the exact documented ownership release sequence.
    for (uint64_t i = 0; i < out_count; ++i) {
        vt->free_value(out_array[i]);
    }
    std::free(out_array);
    BOOST_CHECK_LT(g_live, baseline);

    vt->disconnect(handle);
    inst->shutdown(inst);
}
#endif  // _WIN32
