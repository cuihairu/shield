#define BOOST_TEST_MODULE CovLogger
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "shield/log/logger.hpp"
#include "shield/log/sinks.hpp"

namespace fs = std::filesystem;
namespace log_ns = shield::log;

namespace {

// Shared record store that outlives the sink (Logger::shutdown destroys the
// sinks it owns, so the assertions must not read through a dead sink).
struct SinkState {
    std::vector<log_ns::LogRecord> records;
    int flushes = 0;
};

// Collects every record pushed through the global sink list so tests can
// assert on what the logger actually emitted without touching stdout/stderr.
class CollectingSink : public log_ns::LogSink {
public:
    explicit CollectingSink(std::shared_ptr<SinkState> state)
        : state_(std::move(state)) {}

    void write(const log_ns::LogRecord& r) override {
        state_->records.push_back(r);
    }
    void flush() override { ++state_->flushes; }

private:
    std::shared_ptr<SinkState> state_;
};

// Per-test guard: the logger keeps process-global state (sinks, cached
// loggers, global level). Start every test from a clean slate and tear the
// state down again afterwards so ordering never matters.
struct LoggerReset {
    LoggerReset() { reset(); }
    ~LoggerReset() { reset(); }

    static void reset() {
        log_ns::Logger::shutdown();
        log_ns::Logger::set_global_level(log_ns::Level::Debug);
        log_ns::clear_service_context();
    }
};

fs::path temp_dir() {
    static const fs::path dir =
        fs::temp_directory_path() /
        ("shield_cov_logger_" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

log_ns::LogRecord make_record(log_ns::Level level, std::string name,
                              std::string msg) {
    log_ns::LogRecord r;
    r.level = level;
    r.logger_name = std::move(name);
    r.message = std::move(msg);
    r.timestamp_ms = 1700000000000;
    return r;
}

std::shared_ptr<SinkState> attach_collector() {
    auto state = std::make_shared<SinkState>();
    log_ns::Logger::add_sink(std::make_unique<CollectingSink>(state));
    return state;
}

}  // namespace

// Logs at every level through a FileSink so format_record exercises every
// level_name branch (DEBUG/WARN/FATAL) plus the trace_id suffix, and the
// file/line suffix via the SHIELD_LOG_* macros.
BOOST_FIXTURE_TEST_CASE(all_levels_and_trace_reach_file_sink, LoggerReset) {
    auto dir = temp_dir();
    const auto path = dir / "levels.log";

    log_ns::Logger::add_sink(log_ns::make_file_sink(path.string()));
    log_ns::set_service_context("svc-1", "Auth Service", "trace-42");

    auto& lg = log_ns::get_logger("cov.levels");
    lg.debug("debug msg");
    lg.info("info msg");
    lg.warning("warn msg");
    lg.error("error msg");
    lg.fatal("fatal msg");
    SHIELD_LOG_WARNING(lg, "macro msg");

    log_ns::Logger::shutdown();  // flush + close the file sink

    const auto data = read_file(path);
    BOOST_CHECK(data.find("DEBUG") != std::string::npos);
    BOOST_CHECK(data.find("INFO") != std::string::npos);
    BOOST_CHECK(data.find("WARN") != std::string::npos);
    BOOST_CHECK(data.find("ERROR") != std::string::npos);
    BOOST_CHECK(data.find("FATAL") != std::string::npos);
    BOOST_CHECK(data.find("cov.levels") != std::string::npos);
    BOOST_CHECK(data.find("trace=trace-42") != std::string::npos);
    // The macro passes __FILE__/__LINE__, so the record carries a location.
    BOOST_CHECK(data.find(".cpp:") != std::string::npos);

    fs::remove_all(dir);
}

// set_service_context populates the thread-local context copied into each
// record; clear_service_context empties it again.
BOOST_FIXTURE_TEST_CASE(service_context_propagates_to_records, LoggerReset) {
    auto sink = attach_collector();
    auto& lg = log_ns::get_logger("cov.ctx");

    log_ns::set_service_context("svc-9", "Gateway", "trace-9");
    lg.info("with ctx");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 1u);
    BOOST_CHECK_EQUAL(sink->records[0].service_id, "svc-9");
    BOOST_CHECK_EQUAL(sink->records[0].service_name, "Gateway");
    BOOST_CHECK_EQUAL(sink->records[0].trace_id, "trace-9");

    log_ns::clear_service_context();
    lg.info("without ctx");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 2u);
    BOOST_CHECK(sink->records[1].service_id.empty());
    BOOST_CHECK(sink->records[1].service_name.empty());
    BOOST_CHECK(sink->records[1].trace_id.empty());
}

// get_logger returns the same cached Logger for a repeated name.
BOOST_FIXTURE_TEST_CASE(get_logger_returns_cached_instance, LoggerReset) {
    auto& first = log_ns::get_logger("cov.cache");
    auto& second = log_ns::get_logger("cov.cache");
    BOOST_CHECK_EQUAL(&first, &second);
    BOOST_CHECK_EQUAL(first.name(), "cov.cache");
}

// add_sink(nullptr) is ignored; a real sink added afterwards still receives
// records.
BOOST_FIXTURE_TEST_CASE(add_null_sink_is_ignored, LoggerReset) {
    log_ns::Logger::add_sink(nullptr);

    auto sink = attach_collector();
    log_ns::get_logger("cov.null").info("hello");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 1u);
    BOOST_CHECK_EQUAL(sink->records[0].message, "hello");
}

// Records below the global level are dropped; Logger::set_level forwards to
// the global level.
BOOST_FIXTURE_TEST_CASE(records_below_global_level_are_filtered, LoggerReset) {
    auto sink = attach_collector();
    auto& lg = log_ns::get_logger("cov.filter");

    log_ns::Logger::set_global_level(log_ns::Level::Error);
    lg.debug("nope");
    lg.info("nope");
    lg.warning("nope");
    BOOST_CHECK(sink->records.empty());

    lg.error("kept");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 1u);
    BOOST_CHECK(sink->records[0].level == log_ns::Level::Error);

    lg.set_level(log_ns::Level::Warning);  // rewrites the global level
    lg.info("still nope");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 1u);
    lg.warning("kept too");
    BOOST_REQUIRE_EQUAL(sink->records.size(), 2u);
    BOOST_CHECK(sink->records[1].level == log_ns::Level::Warning);
}

// ConsoleSink: stderr sink, stdout sink, and the level>=Error escalation to
// stderr, plus flush().
BOOST_FIXTURE_TEST_CASE(console_sink_writes_and_flushes, LoggerReset) {
    log_ns::ConsoleSink err_sink(true);
    err_sink.write(
        make_record(log_ns::Level::Warning, "cov.console", "to cerr"));

    log_ns::ConsoleSink out_sink(false);
    out_sink.write(make_record(log_ns::Level::Info, "cov.console", "to cout"));
    out_sink.write(
        make_record(log_ns::Level::Error, "cov.console", "error to cerr"));
    out_sink.flush();
    err_sink.flush();
    BOOST_CHECK(true);  // reached without deadlocking/crashing
}

// A FileSink pointed at an unopenable path silently drops writes/flushes.
BOOST_FIXTURE_TEST_CASE(file_sink_with_unwritable_path_is_silent, LoggerReset) {
    const auto bad = temp_dir() / "missing_dir" / "x.log";
    {
        log_ns::FileSink sink(bad.string());
        sink.write(make_record(log_ns::Level::Info, "cov.file", "dropped"));
        sink.flush();
    }  // destructor runs flush again
    BOOST_CHECK(!fs::exists(bad));
    fs::remove_all(bad.parent_path());
}

// RotatingFileSink rotates once the accumulated size passes max_size, keeps
// the current file small, and leaves rotated files behind.
BOOST_FIXTURE_TEST_CASE(rotating_sink_rotates_on_size, LoggerReset) {
    auto dir = temp_dir();
    const auto base = dir / "rotate.log";
    constexpr size_t kMaxSize = 32;

    {
        auto sink = log_ns::make_rotating_sink(base.string(), kMaxSize,
                                               /*max_files=*/2);
        // First line stays under the limit: rotate_if_needed returns early.
        sink->write(make_record(log_ns::Level::Info, "r", "x"));
        for (int i = 0; i < 5; ++i) {
            sink->write(make_record(log_ns::Level::Info, "cov.rot",
                                    "0123456789_0123456789_0123456789"));
        }
        sink->flush();

        BOOST_CHECK(fs::exists(base));
        BOOST_CHECK(fs::exists(base.string() + ".1"));
        // After the final rotation the base file only holds the last line.
        BOOST_CHECK_LT(fs::file_size(base), 2 * kMaxSize);
        BOOST_CHECK(read_file(base).find("cov.rot") != std::string::npos);
    }  // destructor flush

    // A path with a missing parent directory: the constructor creates the
    // parent so file logging works out of the box (documented default is
    // "logs/shield.log" on a fresh checkout). write/flush must not throw.
    const auto bad = dir / "no_dir" / "x.log";
    {
        auto sink = log_ns::make_rotating_sink(bad.string(), kMaxSize, 0);
        sink->write(make_record(log_ns::Level::Info, "cov.rot", "written"));
        sink->flush();
    }
    BOOST_CHECK(fs::exists(bad));
    BOOST_CHECK(read_file(bad).find("written") != std::string::npos);

    fs::remove_all(dir);
}

// initialize() installs a console sink exactly once; shutdown() flushes and
// clears sinks and cached loggers.
BOOST_FIXTURE_TEST_CASE(initialize_and_shutdown_lifecycle, LoggerReset) {
    log_ns::Logger::shutdown();  // no sinks yet: still fine

    log_ns::Logger::initialize();  // installs the console sink
    log_ns::Logger::initialize();  // sinks non-empty: no second console sink
    log_ns::get_logger("cov.lifecycle").info("to console");
    auto sink = attach_collector();
    log_ns::Logger::shutdown();  // detaches every sink, including ours
    log_ns::get_logger("cov.lifecycle").info("no sinks after shutdown");
    BOOST_CHECK(sink->records.empty());
}

// apply_sinks(false, false, ...) installs a console sink as the fallback so
// records are never silently dropped.
BOOST_AUTO_TEST_CASE(apply_sinks_with_nothing_enabled_falls_back_to_console,
                     *boost::unit_test::timeout(10)) {
    auto sink = attach_collector();
    log_ns::Logger::apply_sinks(/*console=*/false, /*file=*/false, "", 0, 0);
    log_ns::Logger::initialize();  // no-op: sinks already present
    log_ns::get_logger("cov.apply").info("fallback");
    // The collector was detached by apply_sinks; the console fallback sink
    // must not crash and must not resurrect the collector.
    BOOST_CHECK(sink->records.empty());
}

// A RotatingFileSink pointed at a path whose parent cannot be created (under
// /proc) never opens its file; write/flush are silent no-ops.
BOOST_AUTO_TEST_CASE(rotating_sink_unopenable_file_is_silent,
                     *boost::unit_test::timeout(10)) {
    {
        auto sink =
            log_ns::make_rotating_sink("/proc/no-such-dir/x.log", 32, 1);
        sink->write(make_record(log_ns::Level::Info, "cov.rotbad", "dropped"));
        sink->flush();
    }  // dtor flush again
    BOOST_CHECK(true);  // reached without throwing
}
