// Coverage tests for the Lua panic machinery (src/lua/lua_runtime.cpp
// write_lua_panic_forensics + shield_lua_panic): the forensics writer is
// driven in-process against both error-object arms, exec_lua's tostring
// conversion is exercised with raising/user-replaced tostring (the call
// sites that must degrade instead of reaching the aborting panic handler),
// and the abort semantics of the installed handler are verified through a
// fork death test (POSIX only — the child dies by SIGABRT).
//
// Death-test constraint: no CAF actor system and no extra threads are
// created before the fork — fork() must run in a single-threaded process.
#define BOOST_TEST_MODULE CovLuaPanic
#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "lua/lua_panic.hpp"  // src-internal; the suite links shield_lua
#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"

using namespace shield::lua;

namespace {

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// --- In-process: the forensics writer, both error-object arms -------------

BOOST_AUTO_TEST_CASE(ForensicsStringErrorObject) {
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    lua_State* L = lua.lua_state();
    lua_pushliteral(L, "boom-string-msg");
    const std::string detail = write_lua_panic_forensics(L, stderr);
    BOOST_CHECK(detail.find("boom-string-msg") != std::string::npos);
    // The writer restores the stack it was handed (top is unchanged), so
    // the test can pop the error object and leave the state balanced.
    BOOST_CHECK_EQUAL(lua_gettop(L), 1);
    lua_pop(L, 1);
}

BOOST_AUTO_TEST_CASE(ForensicsNonStringErrorObject) {
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    lua_State* L = lua.lua_state();
    lua_createtable(L, 0, 0);
    const std::string detail = write_lua_panic_forensics(L, stderr);
    BOOST_CHECK(detail.find("non-string error object") != std::string::npos);
    BOOST_CHECK(detail.find("table") != std::string::npos);
    BOOST_CHECK_EQUAL(lua_gettop(L), 1);
    lua_pop(L, 1);
}

// --- In-process: exec_lua degrades instead of panicking --------------------

// A table whose __tostring metamethod raises must not reach the (aborting)
// panic handler: the pcall around tostring fails and the slot carries the
// metamethod's error text while the chunk itself still succeeds.
BOOST_AUTO_TEST_CASE(ExecLuaTostringMetamethodErrorDegrades) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    nlohmann::json result;
    std::string error;
    const bool ok =
        runtime.exec_lua(vm,
                         "local t = setmetatable({}, {__tostring = function() "
                         "error('ts-boom') end}) return t, 'fine'",
                         &result, &error);
    BOOST_CHECK(ok);
    BOOST_REQUIRE(result.is_array() && result.size() == 2u);
    BOOST_CHECK(result[0].get<std::string>().find("ts-boom") !=
                std::string::npos);
    BOOST_CHECK_EQUAL(result[1], "fine");
}

// A user-replaced global tostring that returns a non-string: the protected
// call succeeds but lua_tostring yields null — the slot degrades to "".
BOOST_AUTO_TEST_CASE(ExecLuaReplacedTostringReturningTableYieldsEmpty) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    nlohmann::json result;
    std::string error;
    const bool ok = runtime.exec_lua(
        vm, "tostring = function(x) return {} end return {}", &result, &error);
    BOOST_CHECK(ok);
    BOOST_REQUIRE(result.is_array() && result.size() == 1u);
    BOOST_CHECK_EQUAL(result[0], "");
}

// --- Death test: the installed handler aborts after forensics --------------

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(PanicHandlerAbortsAfterForensics) {
    int fds[2];
    BOOST_REQUIRE_EQUAL(pipe(fds), 0);

    const pid_t pid = fork();
    BOOST_REQUIRE_MESSAGE(pid >= 0, "fork failed: " << strerror(errno));
    if (pid == 0) {
        // Child: no BOOST_* assertions here — the framework state is a
        // fork-local copy and a failing assertion would deadlock or
        // double-report. Setup failures exit directly instead.
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);  // forensics stream into the pipe
        close(fds[1]);
        // Boost.Test's execution monitor (catch_system_errors defaults on)
        // installs a SIGABRT handler that fork passes down: without this,
        // abort() would be caught, logged as a fatal error, and the child
        // would exit normally instead of dying by signal.
        std::signal(SIGABRT, SIG_DFL);
        {
            // A fresh runtime installs the panic handler in its ctor. This
            // suite never spawns CAF systems or threads, so the forked
            // address space is single-threaded and safe to initialize in.
            LuaRuntime runtime;
            auto vm = runtime.create_vm();
            sol::state& lua = runtime.vm_state(vm);
            lua.script("function __cov_death() error('death-boom-msg') end");
            lua_State* L = lua.lua_state();
            lua_getglobal(L, "__cov_death");
            lua_call(L, 0, 0);  // truly unprotected: error -> at_panic
        }
        _exit(42);  // regression signal: the handler returned, did not abort
    }

    // Parent: drain the pipe to EOF before reaping. The read end only sees
    // EOF once the child is gone, and waiting first could deadlock if the
    // child's output ever exceeded the pipe buffer.
    close(fds[1]);
    std::string captured;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            captured.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;  // EOF (or unrecoverable read error; assertions below catch it)
    }
    close(fds[0]);

    int status = 0;
    BOOST_REQUIRE_EQUAL(waitpid(pid, &status, 0), pid);
    BOOST_CHECK_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                        "child did not die by SIGABRT (exited normally: the "
                        "panic handler returned)");
    BOOST_CHECK(captured.find("*** shield lua panic ctx:") !=
                std::string::npos);
    BOOST_CHECK(captured.find("*** shield lua panic:") != std::string::npos);
    BOOST_CHECK(captured.find("death-boom-msg") != std::string::npos);
}
#endif  // !_WIN32
