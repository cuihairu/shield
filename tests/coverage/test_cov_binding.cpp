// Coverage tests for include/shield/lua/binding.hpp: the shd registry-ref
// binding layer. Drives the semantic arms of the object/table/function
// views, the table iterator, protected-call error results, load/script
// result arms, the usertype dispatch plumbing (index thunk, method thunk,
// no-construct thunks, gc/property paths), and the argument-conversion
// error machinery (host_type_name + stack_check rejections).
#define BOOST_TEST_MODULE CovBinding
#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"

namespace {

const std::string kTmpDir = "/tmp/shield_cov_binding";

std::string write_file(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool run_script(shd::state& lua, const std::string& code) {
    auto result = lua.script(code);
    if (!result.valid()) {
        const shd::error e = result;
        std::fprintf(stderr, "lua error: %s\n", e.what());
        return false;
    }
    return true;
}

// A locally registered usertype: exercises the native push path, the
// __index method/property dispatch, and the method/self thunk arms.
struct CovBox {
    int v = 5;
    int get() const { return v; }
};

// A usertype whose members throw: drives the exception-conversion arms of
// the usertype call/method thunks (std::exception and non-standard throws).
struct CovBoom {
    int throw_std() const { throw std::runtime_error("method boom"); }
    int throw_odd() const { throw 42; }
};

}  // namespace

namespace shd {
template <>
struct is_usertype_value<CovBoom> : std::true_type {};
}  // namespace shd

namespace shd {
template <>
struct is_usertype_value<CovBox> : std::true_type {};
}  // namespace shd

// ---------------------------------------------------------------------------
// Table iterator arms: empty table (begin lands on end), single pair (++ hits
// the end arm inside the loop), and a two-pair traversal.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TableIterationEmptyAndProgressArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    // Empty table: begin()'s lua_next returns 0 immediately.
    shd::table empty = lua.create_table();
    size_t count = 0;
    for (const auto& kv : empty) {
        (void)kv;
        ++count;
    }
    BOOST_CHECK_EQUAL(count, 0u);

    // Pre-increment on an end iterator: the lua_next guard arm (the
    // sentinel key does not advance).
    auto e = empty.end();
    ++e;

    // Single pair: one iteration, then operator++'s lua_next end arm.
    shd::table one = lua.create_table();
    one["only"] = 1;
    count = 0;
    for (const auto& kv : one) {
        ++count;
        BOOST_CHECK_EQUAL(kv.first.as<std::string>(), "only");
    }
    BOOST_CHECK_EQUAL(count, 1u);

    // Two pairs: the loop body runs twice with live key/value objects.
    shd::table two = lua.create_table();
    two["a"] = 10;
    two["b"] = 20;
    count = 0;
    int sum = 0;
    for (const auto& kv : two) {
        ++count;
        sum += kv.second.as<int>();
    }
    BOOST_CHECK_EQUAL(count, 2u);
    BOOST_CHECK_EQUAL(sum, 30);
}

// ---------------------------------------------------------------------------
// object equality: the invalid/invalid arm (true), the mixed arm (false),
// and the rawequal comparison of two live references.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ObjectEqualityArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::object a;
    shd::object b;
    BOOST_CHECK(a == b);  // both invalid

    lua["x"] = 1;
    shd::object live = lua.get("x");
    BOOST_CHECK(live != a);     // one valid, one invalid
    BOOST_CHECK(!(a == live));  // operator== mixed order (invalid first)

    lua["y"] = 1;
    shd::object same = lua.get("y");
    BOOST_CHECK(live == same);  // rawequal on two (distinct) equal values

    shd::object nil_ref = lua.get("no_such_global");  // LUA_REFNIL reference
    BOOST_CHECK(!nil_ref.valid());
    BOOST_CHECK(nil_ref == a);  // LUA_REFNIL degrades to invalid
}

// ---------------------------------------------------------------------------
// accessor::get_or: the missing-key arm (invalid object), the explicit nil
// arm, and the present-value conversion arm.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AccessorGetOrArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::table t = lua.create_table();
    t["nilkey"] = shd::nil;
    t["present"] = 41;

    BOOST_CHECK_EQUAL(t["missing"].get_or<int>(7), 7);
    BOOST_CHECK_EQUAL(t["nilkey"].get_or<int>(8), 8);
    BOOST_CHECK_EQUAL(t["present"].get_or<int>(0), 41);
    BOOST_CHECK(!t["missing"].valid());
}

// ---------------------------------------------------------------------------
// function::call: the lua_pcall failure arm returns an invalid object, and
// the success arm converts the single return value.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(FunctionCallErrorAndSuccessArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::object boom_obj = lua.script("return function() error('fn boom') end");
    shd::function boom(boom_obj);
    shd::object failed = boom.call();
    BOOST_CHECK(!failed.valid());

    shd::object ok_obj = lua.script("return function(x) return x + 1 end");
    shd::function ok(ok_obj);
    shd::object value = ok.call(41);
    BOOST_REQUIRE(value.valid());
    BOOST_CHECK_EQUAL(value.as<int>(), 42);
}

// ---------------------------------------------------------------------------
// detail::push overloads for reference views: an invalid function/table
// pushes nil instead of dereferencing its (absent) registry entry.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(PushInvalidFunctionAndTablePushNil) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::function bad_fn;
    lua["bad_fn"] = bad_fn;
    shd::table bad_tbl;
    lua["bad_tbl"] = bad_tbl;

    BOOST_CHECK(run_script(lua,
                           "assert(bad_fn == nil)\n"
                           "assert(bad_tbl == nil)"));
}

// ---------------------------------------------------------------------------
// protected_function_result::get_error: the string-message arm, the
// non-string payload arm ("unknown error"), and the empty/null-state arm of
// a result produced by calling an invalid protected function.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ProtectedResultErrorShapes) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    // String message: the LUA_TSTRING arm.
    {
        auto r = lua.script("error('with message')");
        BOOST_CHECK(!r.valid());
        shd::error e = r;
        BOOST_CHECK(std::string(e.what()).find("with message") !=
                    std::string::npos);
    }
    // Non-string error payload: the unknown-error arm of get_error.
    {
        auto r = lua.script("error({code = 1})");
        BOOST_CHECK(!r.valid());
        shd::error e = r;
        BOOST_CHECK(std::string(e.what()).find("unknown") != std::string::npos);
    }
    // An invalid protected_function yields a null-state result whose
    // get_error also reports the unknown error (the L_ == nullptr arm).
    {
        shd::protected_function pf;
        auto r = pf.call();
        BOOST_CHECK(!r.valid());
        shd::error e = r;
        BOOST_CHECK(std::string(e.what()).find("unknown") != std::string::npos);
    }
    // A valid protected call still converts to error as the empty error.
    {
        auto r = lua.script("return 1");
        BOOST_REQUIRE(r.valid());
        shd::error e = r;
        BOOST_CHECK_EQUAL(std::string(e.what()), "");
    }
}

// ---------------------------------------------------------------------------
// load_result arms: a successful load converting to the empty error, a
// syntax failure carrying its message, and the default-constructed shape.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LoadResultConversionArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    {
        shd::load_result lr = lua.load("return 1");
        BOOST_CHECK(lr.valid());
        shd::error e = lr;  // valid() arm: empty error
        BOOST_CHECK_EQUAL(std::string(e.what()), "");
        shd::protected_function pf = lr;
        shd::object ran = pf.call();
        BOOST_CHECK_EQUAL(ran.as<int>(), 1);
    }
    {
        shd::load_result lr = lua.load("x=(");
        BOOST_CHECK(!lr.valid());
        shd::error e = lr;  // value-backed arm: the syntax message
        BOOST_CHECK(std::string(e.what()).size() > 0);
    }
    {
        shd::load_result lr;  // default: invalid status, no value
        BOOST_CHECK(!lr.valid());
        shd::error e = lr;  // !value_.valid() arm: empty error
        BOOST_CHECK_EQUAL(std::string(e.what()), "");
    }
}

// ---------------------------------------------------------------------------
// state_view::script_file: missing file (load failure arm), a script that
// errors at run time (pcall failure arm), and a successful run.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ScriptFileFailureAndSuccessArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::object missing = lua.script_file(kTmpDir + "/no_such_file.lua");
    BOOST_CHECK(!missing.valid());

    const std::string bad =
        write_file("cov_bind_boom.lua", "error('file boom')");
    shd::object errored = lua.script_file(bad);
    BOOST_CHECK(!errored.valid());

    const std::string good = write_file("cov_bind_good.lua", "return 42");
    shd::object value = lua.script_file(good);
    BOOST_REQUIRE(value.valid());
    BOOST_CHECK_EQUAL(value.as<int>(), 42);
}

// ---------------------------------------------------------------------------
// open_libraries with the debug bit (the one library combination the other
// suites never request).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OpenLibrariesIncludesDebug) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string, shd::lib::debug);
    BOOST_CHECK(run_script(lua, "assert(type(debug.debug) == 'function')"));

    shd::state mask_lua;
    mask_lua.open_libraries(shd::lib::base | shd::lib::debug);
    BOOST_CHECK(
        run_script(mask_lua, "assert(type(debug.getinfo) == 'function')"));
}

// ---------------------------------------------------------------------------
// state_view::load_file: the file-mode arm of load_protected (empty code),
// both through success and a load failure.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LoadFileArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    const std::string good = write_file("cov_bind_loadfile.lua", "return 7");
    // load_file is protected dofile semantics: the chunk runs and the
    // result carries its return values.
    shd::protected_function_result r = lua.load_file(good);
    BOOST_REQUIRE(r.valid());
    BOOST_CHECK_EQUAL(r.get<int>(0), 7);

    auto bad = lua.load_file(kTmpDir + "/still_missing.lua");
    BOOST_CHECK(!bad.valid());
    shd::error e = bad;
    BOOST_CHECK(std::string(e.what()).size() > 0);

    // Buffer-mode load failure: the status != LUA_OK arm of load_protected.
    auto syn = lua.script("x=(");
    BOOST_CHECK(!syn.valid());
}

// ---------------------------------------------------------------------------
// state move construction/assignment: the moved-from state's close() runs
// with owned_ == false and does not close the stolen VM.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(StateMoveKeepsStolenVmAlive) {
    shd::state a;
    a.open_libraries(shd::lib::base);
    a["moved"] = 1;

    shd::state b = std::move(a);
    BOOST_CHECK(run_script(b, "assert(moved == 1)"));

    shd::state c;
    c = std::move(b);
    BOOST_CHECK(run_script(c, "assert(moved == 1)"));
    // Self move-assign: the identity guard keeps the live VM in place.
    shd::state& c_alias = c;
    c = std::move(c_alias);
    BOOST_CHECK(run_script(c, "assert(moved == 1)"));
    // The moved-from b closed early; c still owns the live VM.
}

// ---------------------------------------------------------------------------
// The no-construct thunks of the identity usertypes: calling .new() from
// Lua raises the stable Lua error instead of constructing anything.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NoConstructThunksRaise) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shield::lua::register_full_shield_api(lua.lua_state());

    // The identity usertypes register under their literal dotted names
    // (_G["shd.ClientContext"], a single global key, not a nested shd
    // table); ServiceHandle registers bare.
    BOOST_CHECK(
        run_script(lua,
                   "local ok1, e1 = pcall(_G['shd.ClientContext'].new)\n"
                   "assert(not ok1)\n"
                   "assert(tostring(e1):find('unconstructable', 1, true))\n"
                   "local ok2, e2 = pcall(_G['shd.ClientRef'].new)\n"
                   "assert(not ok2)\n"
                   "local ok3, e3 = pcall(ServiceHandle.new)\n"
                   "assert(not ok3)\n"
                   "assert(tostring(e3):find('unconstructable', 1, true))\n"));
}

// ---------------------------------------------------------------------------
// Argument-conversion rejections: one bound function per host type, each
// called with a table (a type no primitive conversion accepts) so every
// host_type_name instantiation runs inside the luaL_error message.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(BadArgumentCarriesHostName) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shd::table g = lua.globals();
    g.set_function("need_bool", [](bool) { return 1; });
    g.set_function("need_int", [](int) { return 1; });
    g.set_function("need_long", [](long) { return 1; });
    g.set_function("need_llong", [](long long) { return 1; });
    g.set_function("need_double", [](double) { return 1; });
    g.set_function("need_string", [](std::string) { return 1; });
    // The full integer-width matrix: every specialization the runtime binds
    // (ports, session ids, timeouts) shares the record line, so each width's
    // reject and accept arms are driven here as well.
    g.set_function("need_i8", [](int8_t) { return 1; });
    g.set_function("need_u8", [](uint8_t) { return 1; });
    g.set_function("need_i16", [](int16_t) { return 1; });
    g.set_function("need_u16", [](uint16_t) { return 1; });
    g.set_function("need_i32", [](int32_t) { return 1; });
    g.set_function("need_u32", [](uint32_t) { return 1; });
    g.set_function("need_u64", [](uint64_t) { return 1; });
    g.set_function("need_short", [](short) { return 1; });
    g.set_function("need_ushort", [](unsigned short) { return 1; });
    g.set_function("need_uint", [](unsigned) { return 1; });
    g.set_function("need_ulong", [](unsigned long) { return 1; });
    g.set_function("need_ullong", [](unsigned long long) { return 1; });
    g.set_function("need_sizet", [](size_t) { return 1; });
    g.set_function("need_float", [](float) { return 1; });

    BOOST_CHECK(run_script(
        lua,
        "local ok, e = pcall(need_bool, {})\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('boolean expected', 1, true))\n"
        "ok, e = pcall(need_int, {})\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('integer expected', 1, true))\n"
        "ok, e = pcall(need_long, {})\n"
        "assert(tostring(e):find('integer expected', 1, true))\n"
        "ok, e = pcall(need_llong, {})\n"
        "assert(tostring(e):find('integer expected', 1, true))\n"
        "ok, e = pcall(need_double, {})\n"
        "assert(tostring(e):find('number expected', 1, true))\n"
        "ok, e = pcall(need_float, {})\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('number expected', 1, true))\n"
        "ok, e = pcall(need_string, {})\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('string expected', 1, true))\n"
        // Success paths across the SSO boundary: the returned std::string
        // exercises both storage arms of the argument copy.
        "assert(need_string('abc') == 1)\n"
        "assert(need_string(string.rep('x', 40)) == 1)\n"
        // Accept arms for the types the width loop does not cover.
        "assert(need_bool(true) == 1)\n"
        "assert(need_long(3) == 1)\n"
        "assert(need_llong(4) == 1)\n"
        // Every integer width accepts a Lua integer and rejects a table.
        "for _, f in ipairs({need_i8, need_u8, need_i16, need_u16, "
        "need_i32, need_u32, need_u64, need_short, need_ushort, "
        "need_uint, need_ulong, need_ullong, need_sizet}) do\n"
        "  assert(f(7) == 1)\n"
        "  local ok2, e2 = pcall(f, {})\n"
        "  assert(not ok2)\n"
        "  assert(tostring(e2):find('integer expected', 1, true))\n"
        "end\n"
        "assert(need_float(1.5) == 1)\n"
        "assert(need_double(2.5) == 1)"));
}

// ---------------------------------------------------------------------------
// Optional parameters and optional-typed casts: the absent-argument arm of
// unpack_arg, and object::as<std::optional<V>> degrading mismatches to the
// empty optional instead of throwing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OptionalParameterAndCastArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shd::table g = lua.globals();
    g.set_function("opt_param",
                   [](std::optional<int> v) { return v.value_or(-1); });
    g.set_function("opt_cast", [](shd::object o) {
        return o.as<std::optional<int>>().value_or(-1);
    });

    BOOST_CHECK(run_script(lua,
                           "assert(opt_param() == -1)\n"  // absent argument
                           "assert(opt_param(5) == 5)\n"
                           "assert(opt_cast('str') == -1)\n"  // mismatch arm
                           "assert(opt_cast(9) == 9)\n"));
}

// ---------------------------------------------------------------------------
// object::as<T> bad-cast arms: the primitive cast error (stack_check reject)
// and the table/function cast error, both raised as Lua errors through pcall.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsBadCastRaisesThroughPcall) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shd::table g = lua.globals();
    g.set_function("cast_int", [](shd::object o) { return o.as<int>(); });
    g.set_function("cast_tbl",
                   [](shd::object o) { return o.as<shd::table>(); });
    // One bad-cast closure per primitive flavor: each host type's reject
    // arm carries its own host_type_name instantiation in the error.
    g.set_function("cast_long", [](shd::object o) { return o.as<long>(); });
    g.set_function("cast_llong",
                   [](shd::object o) { return o.as<long long>(); });
    g.set_function("cast_short", [](shd::object o) { return o.as<short>(); });
    g.set_function("cast_uint", [](shd::object o) { return o.as<unsigned>(); });
    g.set_function("cast_float", [](shd::object o) { return o.as<float>(); });
    g.set_function("cast_double", [](shd::object o) { return o.as<double>(); });
    g.set_function("cast_bool", [](shd::object o) { return o.as<bool>(); });
    g.set_function("cast_string",
                   [](shd::object o) { return o.as<std::string>(); });
    g.set_function("cast_fn",
                   [](shd::object o) { return o.as<shd::function>(); });

    BOOST_CHECK(
        run_script(lua,
                   "local ok, e = pcall(cast_int, 'notanumber')\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('bad cast', 1, true))\n"
                   "assert(tostring(e):find('integer expected', 1, true))\n"
                   "ok, e = pcall(cast_tbl, 42)\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('table expected', 1, true))\n"
                   "for _, f in ipairs({cast_long, cast_llong, cast_short, "
                   "cast_uint, cast_float, cast_double, cast_bool, "
                   "cast_string}) do\n"
                   "  local ok2, e2 = pcall(f, {})\n"
                   "  assert(not ok2)\n"
                   "  assert(tostring(e2):find('bad cast', 1, true))\n"
                   "end\n"
                   "ok, e = pcall(cast_fn, 42)\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('function expected', 1, true))"));
}

// ---------------------------------------------------------------------------
// is<T> arms: an invalid object, a non-userdata value against a usertype,
// and a real CovBox instance resolving through the name/tag registry.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(IsArmsForInvalidAndUsertypes) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);

    shd::object nothing;
    BOOST_CHECK(!nothing.is<shd::table>());  // !valid() arm
    // The !valid() guard across the other is<> dispatch arms: primitives
    // (integral/floating/string) before anything is pushed.
    BOOST_CHECK(!nothing.is<int>());
    BOOST_CHECK(!nothing.is<bool>());
    BOOST_CHECK(!nothing.is<double>());
    BOOST_CHECK(!nothing.is<std::string>());

    shd::object num = lua.script("return 42");
    BOOST_CHECK(!num.is<CovBox>());  // non-userdata arm of usertype_is
    BOOST_CHECK(num.is<int>());      // accept arm
    BOOST_CHECK(num.is<std::string>() == false);  // stack_check reject arm

    // The layout-tag predicate directly: a non-userdata slot short-circuits
    // to false, and a shd-created (tagged) usertype userdata reports true.
    {
        lua_State* L = lua.lua_state();
        lua_pushinteger(L, 7);
        BOOST_CHECK(!shd::detail::is_shd_raw_userdata(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        BOOST_CHECK(!shd::detail::is_shd_raw_userdata(L, -1));
        lua_pop(L, 1);
    }

    shd::new_usertype<CovBox>(lua, "CovBox", "new", shd::no_constructor, "prop",
                              shd::property([]() { return 42; }), "get",
                              &CovBox::get);
    shd::object box = shd::make_userdata<CovBox>(lua, "CovBox", CovBox{});
    BOOST_CHECK(box.is<CovBox>());
    BOOST_CHECK(!box.is<shd::table>());
    // The usertype arm of the !valid() guard (registered type, invalid ref).
    BOOST_CHECK(!nothing.is<CovBox>());
    // Tagged usertype userdata: the uservalue 1 string tag reports true.
    {
        const int idx = box.push();
        BOOST_CHECK(shd::detail::is_shd_raw_userdata(box.state(), idx));
        lua_pop(box.state(), 1);
    }

    // A wrong-tagged userdata: same metatable, no uservalue tag (the
    // tag-absent arm of usertype_is — the metatable alone decides).
    {
        lua_State* L = lua.lua_state();
        (void)lua_newuserdatauv(L, sizeof(CovBox), 0);
        luaL_getmetatable(L, "CovBox");
        lua_setmetatable(L, -2);
        shd::object untagged(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(untagged.is<CovBox>());
    }

    // A bare userdata without any metatable: the metatable-missing arm.
    {
        lua_State* L = lua.lua_state();
        (void)lua_newuserdatauv(L, sizeof(CovBox), 0);
        shd::object bare(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!bare.is<CovBox>());
    }

    // By-value push of a usertype_value (the native push path) and the
    // value/reference read-back arms of as<T>.
    lua["box_by_value"] = CovBox{6};
    shd::object pushed = lua.get("box_by_value");
    BOOST_CHECK(pushed.is<CovBox>());
    CovBox copy = pushed.as<CovBox>();
    BOOST_CHECK_EQUAL(copy.v, 6);
    const CovBox& cref = pushed.as<const CovBox&>();
    BOOST_CHECK_EQUAL(cref.v, 6);

    // This registration's property thunk and no-construct thunk: reading
    // .prop runs the callable dispatch, and .new raises the stable error.
    lua["box"] = box;
    BOOST_CHECK(
        run_script(lua,
                   "assert(box.prop == 42)\n"
                   "local ok, e = pcall(CovBox.new)\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('unconstructable', 1, true))"));

    // A foreign tag on the right metatable: the tag loop runs, matches no
    // registered name, and rejects the value (the tag is authoritative when
    // present - the B1 dual-world seam).
    {
        const int i = box.push();
        lua_pushliteral(box.state(), "not-a-covbox");
        lua_setiuservalue(box.state(), i, 1);
        lua_pop(box.state(), 1);
        BOOST_CHECK(!box.is<CovBox>());
    }
}

// ---------------------------------------------------------------------------
// Throwing usertype members: the callable thunk (property getters) and the
// method thunk convert std::exception messages and non-standard exceptions
// to Lua errors instead of crossing the pcall boundary.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(UsertypeThunkExceptionArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);

    shd::new_usertype<CovBoom>(
        lua, "CovBoom", "new", shd::no_constructor, "std_prop",
        shd::property([]() -> int { throw std::runtime_error("prop boom"); }),
        "odd_prop", shd::property([]() -> int { throw 42; }), "throw_std",
        &CovBoom::throw_std, "throw_odd", &CovBoom::throw_odd);
    shd::object boombox =
        shd::make_userdata<CovBoom>(lua, "CovBoom", CovBoom{});
    lua["boombox"] = boombox;

    // Idempotence: re-registering the same name must not grow the type-name
    // registry, so the is<>/tag lookup loops stay one entry per alias. This
    // also drives the already-present arm of register_type_name's dedup
    // check on this coverage-only instantiation (the production types get
    // it from every repeat VM setup; CovBoom registers once per process).
    shd::register_type_name<CovBoom>("CovBoom");
    BOOST_CHECK_EQUAL(shd::type_names_snapshot<CovBoom>().size(), 1u);

    // A registered-but-different usertype: the name loop finds no match, so
    // usertype_is reports false at the metatable stage.
    BOOST_CHECK(!boombox.is<CovBox>());

    BOOST_CHECK(run_script(
        lua,
        "local ok, e = pcall(function() return boombox.std_prop end)\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('prop boom', 1, true))\n"
        "local ok2, e2 = pcall(function() return boombox.odd_prop end)\n"
        "assert(not ok2)\n"
        "assert(tostring(e2):find('unknown C++ exception', 1, true))\n"
        "local ok3, e3 = pcall(boombox.throw_std, boombox)\n"
        "assert(not ok3)\n"
        "assert(tostring(e3):find('method boom', 1, true))\n"
        "local ok4, e4 = pcall(boombox.throw_odd, boombox)\n"
        "assert(not ok4)\n"
        "assert(tostring(e4):find('unknown C++ exception', 1, true))\n"
        // Bad self: a bound method called with a non-userdata first
        // argument hits the null guard inside the method thunk.
        "local m = boombox.throw_std\n"
        "local okm, em = pcall(m, 42)\n"
        "assert(not okm)\n"
        "assert(tostring(em):find('without a self object', 1, true))\n"
        // This registration's no-construct thunk.
        "local ok5, e5 = pcall(CovBoom.new)\n"
        "assert(not ok5)\n"
        "assert(tostring(e5):find('unconstructable', 1, true))"));
}

// ---------------------------------------------------------------------------
// The usertype __index thunk: method hit, property getter, and missing key;
// plus the method thunk's bad-self arm (calling a bound method with a
// non-userdata first argument).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(UsertypeIndexAndMethodThunkArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);

    shd::new_usertype<CovBox>(
        lua, "CovBox", "new", shd::no_constructor, "prop",
        shd::property([]() { return 42; }), "get", &CovBox::get,
        // A self-shaped callable parameter: the leading const T& goes
        // through self_unpack (the T& argument fast path).
        "add", [](const CovBox& b, int x) { return b.v + x; });
    lua["box"] = shd::make_userdata<CovBox>(lua, "CovBox", CovBox{});

    // Method hit, property getter, missing key (the miss falls through both
    // the methods table and the property table to nil).
    BOOST_CHECK(run_script(lua,
                           "assert(box:get() == 5)\n"
                           "assert(box.prop == 42)\n"
                           "assert(box.nope == nil)\n"
                           "assert(box:add(3) == 8)\n"
                           // The callable pulled off the index and called
                           // with a number self hits the null guard inside
                           // self_unpack.
                           "local addf = box.add\n"
                           "local okb, eb = pcall(addf, 42)\n"
                           "assert(not okb)\n"
                           "assert(tostring(eb):find('bad self argument', 1, "
                           "true))"));

    // Bad self: the method pulled off the index and called with a number in
    // the self slot hits the null-pointer guard inside the method thunk.
    BOOST_CHECK(run_script(
        lua,
        "local m = nil\n"
        "local ok = pcall(function() m = box.get end)\n"
        "assert(ok)\n"
        "local ok2, e = pcall(m, 42)\n"
        "assert(not ok2)\n"
        "assert(tostring(e):find('without a self object', 1, true))"));
}

// ---------------------------------------------------------------------------
// The closure thunk's exception conversion: a std::exception callback maps
// to its message; a non-standard exception maps to "unknown C++ exception".
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClosureThunkExceptionArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shd::table g = lua.globals();
    g.set_function("throw_std",
                   []() -> int { throw std::runtime_error("std boom"); });
    g.set_function("throw_odd", []() -> int { throw 42; });

    BOOST_CHECK(run_script(
        lua,
        "local ok, e = pcall(throw_std)\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('std boom', 1, true))\n"
        "local ok2, e2 = pcall(throw_odd)\n"
        "assert(not ok2)\n"
        "assert(tostring(e2):find('unknown C++ exception', 1, true))"));
}

// ---------------------------------------------------------------------------
// ref_base arms: copy/move assignment guards, the LUA_REFNIL reference, and
// abandon() leaving a reference the destructor must not release.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RefBaseAssignmentAndAbandonArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    // Copying an invalid reference leaves the copy invalid.
    shd::table bad;
    shd::table bad_copy = bad;
    BOOST_CHECK(!bad_copy.valid());
    bad_copy = bad;  // copy-assign from invalid
    BOOST_CHECK(!bad_copy.valid());

    // Self-assign guards (both copy and move forms).
    shd::table t = lua.create_table();
    t["k"] = 1;
    shd::table& t_alias = t;
    t = t_alias;  // self copy-assign: the identity guard arm
    BOOST_CHECK(t.valid());
    shd::table& t_alias2 = t;
    t = std::move(t_alias2);  // self move-assign: the identity guard arm
    BOOST_CHECK(t.valid());
    BOOST_CHECK_EQUAL(t["k"].get_or<int>(0), 1);

    // A moved-from reference releases nothing and stays invalid.
    shd::table stolen = std::move(t);
    BOOST_CHECK(!t.valid());
    BOOST_CHECK(stolen.valid());

    // abandon(): the destructor must skip luaL_unref on the cleared state.
    shd::table abandoned = lua.create_table();
    abandoned.abandon();
    BOOST_CHECK(!abandoned.valid());
}

// ---------------------------------------------------------------------------
// Reference-view conversions: object from (invalid) table/function/
// protected_function/stack_object sources, function from protected_function
// and back, and accessor conversions to table/protected_function.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ReferenceViewConversionGuards) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    // Invalid sources produce invalid objects.
    shd::table it;
    shd::function iff;
    shd::protected_function ipf;
    BOOST_CHECK(!shd::object(it).valid());
    BOOST_CHECK(!shd::object(iff).valid());
    BOOST_CHECK(!shd::object(ipf).valid());
    BOOST_CHECK(!shd::function(ipf).valid());
    BOOST_CHECK(!shd::protected_function(iff).valid());

    // Valid sources re-ref and stay usable.
    shd::object fn_obj = lua.script("return function() return 3 end");
    // Direct-initialization: copy-init from object is ambiguous on clang
    // (both the explicit ctor and the conversion operator are viable there).
    shd::function f(fn_obj);
    shd::protected_function pf = f;
    shd::function round = pf;
    BOOST_CHECK_EQUAL(round.call().as<int>(), 3);
    shd::object fo = f;  // function -> object conversion (valid arm)
    BOOST_CHECK(fo.valid());
    shd::object re_obj = pf;  // object(const protected_function&) valid arm
    BOOST_CHECK(re_obj.valid());
    // as<protected_function> arm of the conversion cascade.
    shd::protected_function pf2 = fn_obj.as<shd::protected_function>();
    BOOST_CHECK_EQUAL(pf2.call().get<int>(0), 3);
    // as<function> arm of the same cascade.
    shd::function ff = fn_obj.as<shd::function>();
    BOOST_CHECK_EQUAL(ff.call().as<int>(), 3);
    // as<object> re-reference arm.
    shd::object self_obj = fn_obj.as<shd::object>();
    BOOST_CHECK(self_obj.valid());

    // A valid table converts to an object and back with content intact.
    shd::table vt = lua.create_table();
    vt["z"] = 3;
    shd::object ovt = vt;
    BOOST_CHECK(ovt.valid());
    BOOST_CHECK_EQUAL(ovt.as<shd::table>()["z"].get_or<int>(0), 3);

    // stack_object conversions (valid slot and nil slot).
    {
        lua_pushinteger(lua.lua_state(), 5);
        shd::stack_object so(shd::state_view(lua), -1);
        shd::object o = so;
        BOOST_CHECK(o.valid());
        BOOST_CHECK_EQUAL(o.as<int>(), 5);
        lua_pop(lua.lua_state(), 1);
    }
    {
        lua_pushnil(lua.lua_state());
        shd::stack_object so(shd::state_view(lua), -1);
        shd::object o = so;  // LUA_REFNIL: the object exists but is invalid
        BOOST_CHECK(!o.valid());
        lua_pop(lua.lua_state(), 1);
    }

    // Accessor conversions: value() backed by a real entry.
    shd::table t = lua.create_table();
    // accessor::operator table re-refs the underlying value, so it needs a
    // table-valued entry (a number entry would be a bad cast).
    shd::table inner = lua.create_table();
    inner["deep"] = 11;
    t["nested"] = inner;
    shd::accessor acc = t["nested"];
    shd::table owner_as_table = acc;  // accessor::operator table
    BOOST_CHECK_EQUAL(owner_as_table["deep"].get_or<int>(0), 11);
    shd::protected_function acc_pf = t["no_fn"];  // nil upgrade stays invalid
    BOOST_CHECK(!acc_pf.valid());
    shd::function acc_fn = t["no_fn"];  // accessor -> function (nil upgrade)
    BOOST_CHECK(!acc_fn.valid());

    // object::operator function(): the valid arm re-refs the registry
    // entry, the invalid arm returns an empty function (static_cast, since
    // copy-init is ambiguous against the explicit ctor on clang).
    shd::function via_conv = static_cast<shd::function>(fn_obj);
    BOOST_CHECK(via_conv.valid());
    BOOST_CHECK_EQUAL(via_conv.call().as<int>(), 3);
    shd::object invalid_obj;
    BOOST_CHECK(!static_cast<shd::function>(invalid_obj).valid());

    // object(const stack_object&) with a default-constructed (state-less)
    // view: the !so.valid() arm produces an invalid object.
    shd::stack_object no_state;
    shd::object from_dead = no_state;
    BOOST_CHECK(!from_dead.valid());

    // protected_function::call on an invalid wrapper: the early-return arm
    // for every argument shape production binds (nothing is pushed before
    // the guard, so the arguments themselves are never touched).
    {
        shd::protected_function bad_pf;
        shd::table arg_t = lua.create_table();
        shd::object arg_o = lua.script("return 1");
        shd::function arg_f = fn_obj;
        const std::string arg_s = "x";
        std::vector<shd::object> many{arg_o, arg_o};
        BOOST_CHECK(!bad_pf.call().valid());
        BOOST_CHECK(!bad_pf.call(std::move(arg_o)).valid());  // object&&
        BOOST_CHECK(!bad_pf.call(arg_t).valid());             // table&
        BOOST_CHECK(!bad_pf.call(arg_f, arg_t).valid());  // function&, table&
        BOOST_CHECK(
            !bad_pf.call(arg_s, arg_t).valid());  // const string&, table&
        BOOST_CHECK(!bad_pf.call(shd::as_args(many)).valid());  // as_args pack
    }

    // function::call failure arm for the argument shapes production binds:
    // the pcall error path pops the message and returns an invalid object.
    {
        shd::object boom_fn_obj =
            lua.script("return function() error('shape boom') end");
        shd::function fail_fn(boom_fn_obj);
        BOOST_CHECK(!fail_fn.call().valid());
        BOOST_CHECK(!fail_fn.call(std::string("x")).valid());
        BOOST_CHECK(!fail_fn.call(41).valid());
    }
}

// ---------------------------------------------------------------------------
// protected_function_result::get<T> across the conversion cascade.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ProtectedResultGetConversions) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    {
        auto r = lua.script("return 5");
        BOOST_CHECK_EQUAL(r.get<int>(0), 5);
    }
    {
        auto r = lua.script("return 'str'");
        BOOST_CHECK_EQUAL(r.get<std::string>(0), "str");
    }
    {
        auto r = lua.script("return {a = 1}");
        shd::table t = r.get<shd::table>(0);
        BOOST_CHECK_EQUAL(t["a"].get_or<int>(0), 1);
    }
    {
        auto r = lua.script("return 1, 2");
        BOOST_CHECK(r.get<shd::object>(1).valid());
    }
}

// ---------------------------------------------------------------------------
// push_result specializations: a table-valued return (the usertype_value
// push path), a multi-value tuple return (the pack fold), and a void return.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(BoundReturnArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
    shd::table g = lua.globals();

    shd::table proto = lua.create_table();
    proto["v"] = 9;
    g.set_function("ret_table", [proto]() { return proto; });
    g.set_function("ret_pair",
                   []() { return std::make_tuple(1, std::string("x")); });
    g.set_function("ret_void", []() {});
    // The arithmetic return spellings the suites never produce: each one
    // instantiates its own result-push pair (long/short/long long resolve to
    // the enable_if arithmetic template, float to the dedicated overload),
    // and a returned function value pushes through the function view.
    g.set_function("ret_long", []() -> long { return 7; });
    g.set_function("ret_llong", []() -> long long { return 8; });
    g.set_function("ret_short", []() -> short { return 9; });
    g.set_function("ret_float", []() -> float { return 1.5f; });
    g.set_function("ret_double", []() -> double { return 2.5; });
    shd::object fn_obj = lua.script("return function() return 4 end");
    shd::function stored(fn_obj);
    g.set_function("ret_fn", [stored]() -> shd::function { return stored; });

    BOOST_CHECK(run_script(lua,
                           "local t = ret_table()\n"
                           "assert(t.v == 9)\n"
                           "local a, b = ret_pair()\n"
                           "assert(a == 1 and b == 'x')\n"
                           "ret_void()\n"
                           "assert(ret_long() == 7)\n"
                           "assert(ret_llong() == 8)\n"
                           "assert(ret_short() == 9)\n"
                           "assert(ret_float() == 1.5)\n"
                           "assert(ret_double() == 2.5)\n"
                           "assert(type(ret_fn()) == 'function')\n"
                           "assert(ret_fn()() == 4)"));
}

// ---------------------------------------------------------------------------
// The runtime's own ServiceHandle usertype through the is<T> name/tag
// registry: non-userdata reject, bare userdata (metatable-missing), a real
// tagged instance, and a mismatched metatable.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ServiceHandleIsArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    shield::lua::ServiceHandle::register_usertype(lua);

    shd::object num = lua.script("return 42");
    BOOST_CHECK(!num.is<shield::lua::ServiceHandle>());  // non-userdata arm

    // The !valid() guard on the usertype arm of is<> (registered type).
    shd::object nothing;
    BOOST_CHECK(!nothing.is<shield::lua::ServiceHandle>());

    shield::lua::ServiceHandle handle("cov-svc");
    shd::object h = shd::make_userdata<shield::lua::ServiceHandle>(
        lua, "ServiceHandle", handle);
    BOOST_CHECK(h.is<shield::lua::ServiceHandle>());

    // A bare userdata (no metatable at all): the metatable-missing arm.
    {
        lua_State* L = lua.lua_state();
        (void)lua_newuserdatauv(L, sizeof(handle), 0);
        shd::object bare(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!bare.is<shield::lua::ServiceHandle>());
    }

    // The registered metatable without the shd layout tag: the metatable
    // decides (uservalue 1 holds no string, so the tag check is skipped).
    // A live ServiceHandle is placement-constructed inside: the metatable
    // carries the __gc finalizer, and an uninitialized payload would crash
    // it at collection time.
    {
        lua_State* L = lua.lua_state();
        (void)lua_newuserdatauv(L, sizeof(handle), 0);
        new (lua_touserdata(L, -1)) shield::lua::ServiceHandle(handle);
        luaL_getmetatable(L, "ServiceHandle");
        lua_setmetatable(L, -2);
        shd::object untagged(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(untagged.is<shield::lua::ServiceHandle>());
    }

    // Metamethod callables pulled off the metatable and invoked with a
    // non-userdata self: the null guard inside self_unpack rejects them.
    lua["handle"] = h;
    BOOST_CHECK(
        run_script(lua,
                   "local mt = getmetatable(handle)\n"
                   "local ok1, e1 = pcall(mt.__tostring, 42)\n"
                   "assert(not ok1)\n"
                   "assert(tostring(e1):find('bad self argument', 1, true))\n"
                   "local ok2, e2 = pcall(mt.__eq, 42, 42)\n"
                   "assert(not ok2)\n"
                   "assert(tostring(e2):find('bad self argument', 1, true))"));

    // Method thunks reject a non-userdata self too (the id/node/valid
    // member-pointer closures this registration installs).
    BOOST_CHECK(run_script(
        lua,
        "local m = handle.id\n"
        "local ok1, e1 = pcall(m, 42)\n"
        "assert(not ok1)\n"
        "assert(tostring(e1):find('without a self object', 1, true))\n"
        "local v = handle.valid\n"
        "local ok2, e2 = pcall(v, 42)\n"
        "assert(not ok2)\n"
        "assert(tostring(e2):find('without a self object', 1, true))"));

    // By-value push of the handle (the usertype_value push path).
    lua["by_value"] = handle;
    shd::object pushed = lua.get("by_value");
    BOOST_CHECK(pushed.is<shield::lua::ServiceHandle>());

    // A userdata whose metatable matches no registered name: the name loop
    // exhausts without a match and the metatable stage rejects the value.
    {
        lua_State* L = lua.lua_state();
        lua_newuserdatauv(L, sizeof(handle), 1);
        luaL_newmetatable(L, "cov_handle_mismatch");
        lua_setmetatable(L, -2);
        shd::object mm(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!mm.is<shield::lua::ServiceHandle>());
    }

    // The right metatable carrying a foreign tag: the tag loop matches no
    // name and rejects the value (the tag decides when present). The
    // metatable is registered under a fresh name so the metatable stage
    // itself succeeds - and stays __gc-free, because this userdata is raw
    // storage with no ServiceHandle constructed inside.
    {
        lua_State* L = lua.lua_state();
        shd::register_type_name<shield::lua::ServiceHandle>(
            "cov_handle_tagged");
        lua_newuserdatauv(L, sizeof(handle), 1);
        luaL_newmetatable(L, "cov_handle_tagged");
        lua_setmetatable(L, -2);
        lua_pushliteral(L, "not-a-handle");
        lua_setiuservalue(L, -2, 1);
        shd::object tagged(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!tagged.is<shield::lua::ServiceHandle>());
    }
}

// ---------------------------------------------------------------------------
// The client identity boxes through the is<> name/tag registry: the invalid
// guard, a bare userdata (no metatable), a mismatched metatable, and a
// foreign tag on the right metatable (the tag decides).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientBoxIsArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::string);
    lua_State* L = lua.lua_state();

    using shield::lua::ClientContextBox;
    using shield::lua::ClientRefBox;

    // Invalid object: the class-branch guard returns false before anything
    // is pushed.
    shd::object nothing;
    BOOST_CHECK(!nothing.is<ClientContextBox>());
    BOOST_CHECK(!nothing.is<ClientRefBox>());

    // A bare userdata without a metatable: the lua_getmetatable arm of the
    // metatable stage.
    {
        (void)lua_newuserdatauv(L, sizeof(ClientContextBox), 1);
        shd::object bare(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!bare.is<ClientContextBox>());
        BOOST_CHECK(!bare.is<ClientRefBox>());
    }

    // A userdata whose metatable matches no registered name: the name loop
    // exhausts without a match and the metatable stage rejects the value.
    {
        lua_newuserdatauv(L, sizeof(ClientRefBox), 1);
        luaL_newmetatable(L, "cov_box_mismatch");
        lua_setmetatable(L, -2);
        shd::object mm(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!mm.is<ClientRefBox>());
        BOOST_CHECK(!mm.is<ClientContextBox>());
    }

    // The right metatable carrying a foreign tag: the tag loop matches no
    // registered name and rejects the value. The metatable and its registry
    // slot are created here together with the type-name registration, so
    // the comparison succeeds regardless of test ordering.
    {
        shd::register_type_name<ClientRefBox>("cov_client_ref");
        lua_newuserdatauv(L, sizeof(ClientRefBox), 1);
        luaL_newmetatable(L, "cov_client_ref");
        lua_setmetatable(L, -2);
        lua_pushliteral(L, "not-a-clientref");
        lua_setiuservalue(L, -2, 1);
        shd::object tagged(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!tagged.is<ClientRefBox>());
    }

    // Same foreign-tag rejection for the context box (each registered type
    // instantiates its own copy of the tag loop).
    {
        shd::register_type_name<ClientContextBox>("cov_client_ctx");
        lua_newuserdatauv(L, sizeof(ClientContextBox), 1);
        luaL_newmetatable(L, "cov_client_ctx");
        lua_setmetatable(L, -2);
        lua_pushliteral(L, "not-a-context");
        lua_setiuservalue(L, -2, 1);
        shd::object tagged_ctx(L, -1);
        lua_pop(L, 1);
        BOOST_CHECK(!tagged_ctx.is<ClientContextBox>());
    }
}

// ---------------------------------------------------------------------------
// make_object helpers: the value/reference/nil shapes over the host type
// set (string double, long, bool, and the nil overload).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MakeObjectShapeMatrix) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    std::string str = "mk";
    double dv = 1.5;
    long lv = 7;
    bool bv = true;

    shd::object o1 = shd::make_object(lua.lua_state(), str);   // string&
    shd::object o2 = shd::make_object(lua.lua_state(), dv);    // double&
    shd::object o3 = shd::make_object(lua.lua_state(), lv);    // long&
    shd::object o4 = shd::make_object(lua.lua_state(), bv);    // bool&
    shd::object o5 = shd::make_object(lua.lua_state(), true);  // bool&&
    shd::object o6 = shd::make_object(lua.lua_state(), shd::nil);

    BOOST_CHECK(o1.valid());
    BOOST_CHECK_EQUAL(o1.as<std::string>(), "mk");
    BOOST_CHECK_EQUAL(o2.as<double>(), 1.5);
    BOOST_CHECK_EQUAL(o3.as<long>(), 7L);
    BOOST_CHECK(o4.as<bool>());
    BOOST_CHECK(o5.as<bool>());
    BOOST_CHECK(!o6.valid());  // nil refs degrade to invalid
}

// ---------------------------------------------------------------------------
// as<T> across the primitive conversion cascade: every arithmetic read arm
// converts a live integer object (and bool coerces nonzero to true).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsPrimitiveMatrix) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
    shd::object num = lua.script("return 42");

    BOOST_CHECK_EQUAL(num.as<int>(), 42);
    BOOST_CHECK_EQUAL(num.as<long>(), 42L);
    BOOST_CHECK_EQUAL(num.as<long long>(), 42LL);
    BOOST_CHECK_EQUAL(num.as<short>(), short(42));
    BOOST_CHECK_EQUAL(num.as<unsigned>(), 42u);
    BOOST_CHECK_EQUAL(num.as<float>(), 42.0f);
    BOOST_CHECK_EQUAL(num.as<double>(), 42.0);
    BOOST_CHECK(!num.is<bool>());  // stack_check: numbers are not booleans
    shd::object yes = lua.script("return true");
    BOOST_CHECK(yes.as<bool>());

    shd::object str = lua.script("return '7'");
    BOOST_CHECK_EQUAL(str.as<std::string>(), "7");
}
