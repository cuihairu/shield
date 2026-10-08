// [SHIELD] Crash collector (Crashpad) glue implementation.
// Design contract: docs/crash-reporting.md. Compiled only when
// SHIELD_ENABLE_CRASHPAD is on (CMake adds this TU conditionally), so it
// stays out of the gcovr filter surface in coverage builds.
#include "shield/crash/crash.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "base/files/file_path.h"
#include "client/crashpad_client.h"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/version.hpp"

namespace shield::crash {
namespace {

namespace fs = std::filesystem;

// Default dump directory relative to the working directory.
constexpr const char* kDefaultDumpDir = "crash";
constexpr const char* kHandlerExecutable = "crashpad_handler";

fs::path resolve_handler_path(const std::string& configured) {
    if (!configured.empty()) {
        return fs::path(configured);
    }
    // Default: the handler shipped next to the running executable.
    // /proc/self/exe stays correct through symlinks and regardless of how
    // the process was started.
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return self.parent_path() / kHandlerExecutable;
}

std::vector<fs::path> scan_pending_dumps(const fs::path& dump_dir) {
    std::vector<fs::path> dumps;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dump_dir, ec)) {
        std::error_code file_ec;
        if (entry.is_regular_file(file_ec) &&
            entry.path().extension() == ".dmp") {
            dumps.push_back(entry.path());
        }
    }
    std::sort(dumps.begin(), dumps.end());
    return dumps;
}

}  // namespace

bool initialize() {
    auto& log = shield::log::get_logger("crash");

    if (shield::config::get("crash.enabled", "false") != "true") {
        return false;
    }

    const fs::path handler =
        resolve_handler_path(shield::config::get("crash.handler_path", ""));
    std::error_code ec;
    if (handler.empty() || !fs::exists(handler, ec)) {
        SHIELD_LOG_ERROR(
            log, "crash collector degraded: crashpad handler not found at '" +
                     handler.string() +
                     "' (set crash.handler_path or ship crashpad_handler "
                     "next to the shield binary)");
        return false;
    }

    // Upload slot is reserved but not implemented (docs/crash-reporting.md:
    // external send stays off by default). Warn so nobody assumes reports
    // are being sent.
    const std::string upload_url = shield::config::get("crash.upload_url", "");
    if (!upload_url.empty()) {
        SHIELD_LOG_WARNING(
            log,
            "crash.upload_url is set but upload is not implemented; "
            "crash dumps stay local: " +
                upload_url);
    }

    const std::string dump_dir_raw = shield::config::get("crash.dump_dir", "");
    const fs::path dump_dir = dump_dir_raw.empty() ? fs::path(kDefaultDumpDir)
                                                   : fs::path(dump_dir_raw);
    fs::create_directories(dump_dir, ec);
    if (ec) {
        SHIELD_LOG_ERROR(
            log, "crash collector degraded: cannot create dump directory '" +
                     dump_dir.string() + "': " + ec.message());
        return false;
    }

    // Dumps left from a previous run are the operator-facing crash signal.
    const std::vector<fs::path> pending = scan_pending_dumps(dump_dir);
    if (!pending.empty()) {
        std::string names;
        for (const auto& dump : pending) {
            names += " " + dump.filename().string();
        }
        SHIELD_LOG_WARNING(log, std::to_string(pending.size()) +
                                    " pending crash dump(s) in '" +
                                    dump_dir.string() + "':" + names);
    }

    crashpad::CrashpadClient client;
    const std::map<std::string, std::string> annotations{
        {"version", shield::get_version_string()},
    };
    // url empty: no upload (handler only writes minidumps locally).
    // Synchronous start so the collector is armed before bootstrap systems
    // come up; restartable so a dead handler is relaunched on next crash.
    const bool started = client.StartHandler(
        base::FilePath{handler.string()}, base::FilePath{dump_dir.string()},
        base::FilePath{dump_dir.string()}, "", annotations, {},
        /*restartable=*/true,
        /*asynchronous_start=*/false);
    if (!started) {
        SHIELD_LOG_ERROR(log,
                         "crash collector degraded: failed to start handler '" +
                             handler.string() + "'");
        return false;
    }

    SHIELD_LOG_INFO(log, "crash collector armed: handler=" + handler.string() +
                             " dump_dir=" + dump_dir.string());
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
[[noreturn]] void crash_test_null_deref() {
    volatile int* null_address = nullptr;
    *null_address = 1;  // deliberate SIGSEGV for --crash-test acceptance
    std::abort();       // backstop if the write ever stops trapping
}

}  // namespace shield::crash
