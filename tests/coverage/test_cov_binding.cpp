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

#include "shield/lua/binding.hpp"
#include "shield/lua/lua_api.hpp"

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
    BOOST_CHECK(live != a);  // one valid, one invalid

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
    shd::protected_function_result r = lua.load_file(good);
    BOOST_REQUIRE(r.valid());
    shd::object ran = static_cast<shd::protected_function>(r).call();
    BOOST_CHECK_EQUAL(ran.as<int>(), 7);

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
    // The moved-from b closed early; c still owns the live VM.
}

// ---------------------------------------------------------------------------
// The no-construct thunks of the identity usertypes: calling .new() from
// Lua raises the stable Lua error instead of constructing anything.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NoConstructThunksRaise) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
    shield::lua::register_full_shield_api(lua);

    BOOST_CHECK(
        run_script(lua,
                   "local ok1, e1 = pcall(shd.ClientContext.new)\n"
                   "assert(not ok1)\n"
                   "assert(tostring(e1):find('unconstructable', 1, true))\n"
                   "local ok2, e2 = pcall(shd.ClientRef.new)\n"
                   "assert(not ok2)\n"
                   "local ok3, e3 = pcall(ServiceHandle.new)\n"
                   "assert(not ok3)\n"));
}

// ---------------------------------------------------------------------------
// Argument-conversion rejections: one bound function per host type, each
// called with a table (a type no primitive conversion accepts) so every
// host_type_name instantiation runs inside the luaL_error message.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(BadArgumentCarriesHostName) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
    shd::table g = lua.globals();
    g.set_function("need_bool", [](bool) { return 1; });
    g.set_function("need_int", [](int) { return 1; });
    g.set_function("need_long", [](long) { return 1; });
    g.set_function("need_llong", [](long long) { return 1; });
    g.set_function("need_double", [](double) { return 1; });
    g.set_function("need_string", [](std::string) { return 1; });

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
        "ok, e = pcall(need_string, {})\n"
        "assert(not ok)\n"
        "assert(tostring(e):find('string expected', 1, true))\n"
        // Success paths across the SSO boundary: the returned std::string
        // exercises both storage arms of the argument copy.
        "assert(need_string('abc') == 1)\n"
        "assert(need_string(string.rep('x', 40)) == 1)"));
}

// ---------------------------------------------------------------------------
// Optional parameters and optional-typed casts: the absent-argument arm of
// unpack_arg, and object::as<std::optional<V>> degrading mismatches to the
// empty optional instead of throwing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OptionalParameterAndCastArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
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
    lua.open_libraries(shd::lib::base);
    shd::table g = lua.globals();
    g.set_function("cast_int", [](shd::object o) { return o.as<int>(); });
    g.set_function("cast_tbl",
                   [](shd::object o) { return o.as<shd::table>(); });

    BOOST_CHECK(
        run_script(lua,
                   "local ok, e = pcall(cast_int, 'notanumber')\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('bad cast', 1, true))\n"
                   "assert(tostring(e):find('integer expected', 1, true))\n"
                   "ok, e = pcall(cast_tbl, 42)\n"
                   "assert(not ok)\n"
                   "assert(tostring(e):find('table expected', 1, true))"));
}

// ---------------------------------------------------------------------------
// is<T> arms: an invalid object, a non-userdata value against a usertype,
// and a real CovBox instance resolving through the name/tag registry.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(IsArmsForInvalidAndUsertypes) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::object nothing;
    BOOST_CHECK(!nothing.is<shd::table>());  // !valid() arm

    shd::object num = lua.script("return 42");
    BOOST_CHECK(!num.is<CovBox>());  // non-userdata arm of usertype_is
    BOOST_CHECK(!num.is<int>());     // stack_check reject arm
    BOOST_CHECK(num.is<std::string>() == false);

    shd::new_usertype<CovBox>(lua, "CovBox", "new", shd::no_constructor, "prop",
                              shd::property([]() { return 42; }), "get",
                              &CovBox::get);
    shd::object box = shd::make_userdata<CovBox>(lua, "CovBox", CovBox{});
    BOOST_CHECK(box.is<CovBox>());
    BOOST_CHECK(!box.is<shd::table>());

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
}

// ---------------------------------------------------------------------------
// Throwing usertype members: the callable thunk (property getters) and the
// method thunk convert std::exception messages and non-standard exceptions
// to Lua errors instead of crossing the pcall boundary.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(UsertypeThunkExceptionArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::new_usertype<CovBoom>(
        lua, "CovBoom", "new", shd::no_constructor, "std_prop",
        shd::property([]() -> int { throw std::runtime_error("prop boom"); }),
        "odd_prop", shd::property([]() -> int { throw 42; }), "throw_std",
        &CovBoom::throw_std, "throw_odd", &CovBoom::throw_odd);
    shd::object boombox =
        shd::make_userdata<CovBoom>(lua, "CovBoom", CovBoom{});
    lua["boombox"] = boombox;

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
        "assert(tostring(e4):find('unknown C++ exception', 1, true))"));
}

// ---------------------------------------------------------------------------
// The usertype __index thunk: method hit, property getter, and missing key;
// plus the method thunk's bad-self arm (calling a bound method with a
// non-userdata first argument).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(UsertypeIndexAndMethodThunkArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);

    shd::new_usertype<CovBox>(lua, "CovBox", "new", shd::no_constructor, "prop",
                              shd::property([]() { return 42; }), "get",
                              &CovBox::get);
    lua["box"] = shd::make_userdata<CovBox>(lua, "CovBox", CovBox{});

    // Method hit, property getter, missing key (the miss falls through both
    // the methods table and the property table to nil).
    BOOST_CHECK(run_script(lua,
                           "assert(box:get() == 5)\n"
                           "assert(box.prop == 42)\n"
                           "assert(box.nope == nil)"));

    // Bad self: the method pulled off the index and called with a number in
    // the self slot hits the null-pointer guard inside the method thunk.
    BOOST_CHECK(
        run_script(lua,
                   "local m = nil\n"
                   "local ok = pcall(function() m = box.get end)\n"
                   "assert(ok)\n"
                   "local ok2, e = pcall(m, 42)\n"
                   "assert(not ok2)\n"
                   "assert(tostring(e):find('bad self argument', 1, true))"));
}

// ---------------------------------------------------------------------------
// The closure thunk's exception conversion: a std::exception callback maps
// to its message; a non-standard exception maps to "unknown C++ exception".
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClosureThunkExceptionArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base);
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
    shd::object re_obj = pf;  // object(const protected_function&) valid arm
    BOOST_CHECK(re_obj.valid());
    // as<protected_function> arm of the conversion cascade.
    shd::protected_function pf2 = fn_obj.as<shd::protected_function>();
    BOOST_CHECK_EQUAL(pf2.call().get<int>(0), 3);
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
    t["inner"] = 11;
    shd::accessor acc = t["inner"];
    shd::table owner_as_table = acc;  // accessor::operator table
    BOOST_CHECK_EQUAL(owner_as_table["inner"].get_or<int>(0), 11);
    shd::protected_function acc_pf = t["no_fn"];  // nil upgrade stays invalid
    BOOST_CHECK(!acc_pf.valid());
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
