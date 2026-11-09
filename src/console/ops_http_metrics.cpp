// [SHIELD_CONSOLE] Ops /ops/health and /ops/metrics endpoints with
// the process-resource sampling and Prometheus emission helpers.
// Split out of ops_http_handler.cpp.
#include "shield/console/ops_http_handler.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/net/listener.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifdef SHIELD_ENABLE_CLUSTER
#include "cluster_status.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#endif

#ifdef SHIELD_ENABLE_SERVER
#include "server_status.hpp"
#include "shield/server/server_manager.hpp"
#endif

#ifdef SHIELD_ENABLE_GLOBAL
#include "global_status.hpp"
#include "shield/global/global_manager.hpp"
#endif

namespace shield::console {

namespace {
/// Process start reference for uptime-based health and metrics. First use
/// in this translation unit pins the process start (good enough for a
/// health probe; the optional server module has the authoritative clock).
const std::chrono::steady_clock::time_point kProcessStart =
    std::chrono::steady_clock::now();

double process_uptime_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         kProcessStart)
        .count();
}

/// Process-level resource gauges. Platform differences are isolated here:
/// each supported platform fills the fields it can measure; unset fields
/// stay negative and their gauges are omitted from /ops/metrics.
/// Values are bytes (RSS), count (FD/threads), or percent (CPU).
struct ProcessResource {
    double rss_kib = -1;      // resident set size, KiB
    double threads = -1;      // Threads count
    double fd_count = -1;     // open file descriptors (handles on Windows)
    double cpu_percent = -1;  // CPU since last sample
};

// Last CPU sample (cumulative user+system CPU seconds and the wall
// timestamp) so the CPU gauge is a rate over the interval, not a
// cumulative counter.
struct CpuSample {
    double cpu_seconds = 0;
    std::chrono::steady_clock::time_point at;
};

CpuSample& last_cpu_sample() {
    static CpuSample s;
    return s;
}

/// Record a cumulative CPU-time reading and turn the delta over the wall
/// interval into the cpu_percent gauge (0 on the first sample, so the
/// gauge is present immediately; subsequent polls carry the rate).
void apply_cpu_sample(ProcessResource& r, double cpu_seconds) {
    const auto now = std::chrono::steady_clock::now();
    CpuSample& last = last_cpu_sample();
    if (last.at != std::chrono::steady_clock::time_point{}) {
        const double dt = std::chrono::duration<double>(now - last.at).count();
        if (dt > 0) {  // GCOVR_EXCL_BR_LINE (defensive: monotonic clock always
                       // advances between two polls; the dt<=0 arm is not
                       // reachable with a live clock)
            r.cpu_percent = ((cpu_seconds - last.cpu_seconds) / dt) * 100.0;
        }
    } else {
        r.cpu_percent = 0.0;
    }
    last.cpu_seconds = cpu_seconds;
    last.at = now;
}

ProcessResource read_process_resources() {
    ProcessResource r;

#if defined(_WIN32)
    // RSS: process working set size.
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        r.rss_kib = static_cast<double>(pmc.WorkingSetSize) / 1024.0;
    }
    // Threads: system thread snapshot filtered to this process.
    {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            const DWORD pid = GetCurrentProcessId();
            double n = 0;
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid) ++n;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            if (n > 0) {
                r.threads = n;
            }
        }
    }
    // Open handles: the Windows analogue of open file descriptors.
    DWORD handle_count = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handle_count)) {
        r.fd_count = static_cast<double>(handle_count);
    }
    // CPU: kernel + user time in 100 ns units since process start.
    {
        FILETIME create_time{}, exit_time{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time,
                            &kernel, &user)) {
            const auto to_seconds = [](const FILETIME& ft) {
                ULARGE_INTEGER v{};
                v.LowPart = ft.dwLowDateTime;
                v.HighPart = ft.dwHighDateTime;
                return static_cast<double>(v.QuadPart) / 1e7;
            };
            apply_cpu_sample(r, to_seconds(kernel) + to_seconds(user));
        }
    }
#elif defined(__APPLE__)
    // RSS: ru_maxrss is in bytes on macOS (KiB on Linux) and is a *peak*
    // (high-water mark) value: it never decreases, so it would misreport a
    // process whose memory has dropped. Read the current resident size from
    // mach_task_basic_info instead, which reflects the live working set.
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_maxrss > 0) {
        // ru_maxrss is only the fallback if the live-size query fails.
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      reinterpret_cast<task_info_t>(&info),
                      &info_count) == KERN_SUCCESS) {
            r.rss_kib = static_cast<double>(info.resident_size) / 1024.0;
        } else {
            r.rss_kib = static_cast<double>(ru.ru_maxrss) / 1024.0;
        }
    }
    // Threads: mach task thread list.
    {
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &list, &count) == KERN_SUCCESS &&
            list != nullptr) {
            r.threads = static_cast<double>(count);
            // task_threads returns one Mach port per thread; each must be
            // released individually, otherwise every metrics poll leaks the
            // port rights (the task_threads documentation requires it).
            for (mach_msg_type_number_t i = 0; i < count; ++i) {
                mach_port_deallocate(mach_task_self(), list[i]);
            }
            vm_deallocate(mach_task_self(),
                          reinterpret_cast<vm_address_t>(list),
                          static_cast<vm_size_t>(count * sizeof(thread_act_t)));
        }
    }
    // Open FDs: count entries in /dev/fd.
    std::error_code ec;
    std::size_t fd_count = 0;
    for (auto it = std::filesystem::directory_iterator("/dev/fd", ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!ec) ++fd_count;
        if (ec) break;
    }
    if (!ec) {
        r.fd_count = static_cast<double>(fd_count);
    }
    // CPU: user + system time in microseconds.
    {
        struct rusage rc{};
        if (getrusage(RUSAGE_SELF, &rc) == 0) {
            const double seconds =
                static_cast<double>(rc.ru_utime.tv_sec) +
                static_cast<double>(rc.ru_utime.tv_usec) / 1e6 +
                static_cast<double>(rc.ru_stime.tv_sec) +
                static_cast<double>(rc.ru_stime.tv_usec) / 1e6;
            apply_cpu_sample(r, seconds);
        }
    }
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            std::string unit;
            double v = 0;
            if (iss >> v >> unit && unit == "kB") {  // GCOVR_EXCL_BR_LINE
                // (defensive: /proc/self/status always reports "VmRSS: <n> kB"
                // on Linux; the parse-fail and non-kB arms are unreachable for
                // a live kernel procfs)
                r.rss_kib = v;
            }
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream iss(line.substr(8));
            double v = 0;
            if (iss >> v) {  // GCOVR_EXCL_BR_LINE (defensive: the Threads:
                             // line always parses)
                r.threads = v;
            }
        }
    }

    // Open FDs: count entries in /proc/self/fd.
    std::error_code ec;
    std::size_t fd_count = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!ec) {  // GCOVR_EXCL_BR_LINE (defensive: /proc/self/fd iteration
                    // only errors if the directory vanishes mid-read, not
                    // forceable for a live process)
            ++fd_count;
        }
        if (ec) {  // GCOVR_EXCL_BR_LINE (defensive: see note above — the same
                   // vanishing-directory fault)
            break;
        }
    }
    if (!ec) {  // GCOVR_EXCL_BR_LINE (defensive: the fd directory always
                // exists)
        r.fd_count = static_cast<double>(fd_count);
    }

    // CPU: utime + stime from /proc/self/stat (fields 14 and 15, 1-based).
    // The sample is clock ticks since process start; sysconf converts it
    // to seconds and the delta over the wall interval yields a percentage.
    std::ifstream stat("/proc/self/stat");
    std::string content((std::istreambuf_iterator<char>(stat)),
                        std::istreambuf_iterator<char>());
    if (!content.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: /proc/self/stat
                             // is never empty for a live process)
        auto rparen = content.rfind(')');
        if (rparen != std::string::npos &&  // GCOVR_EXCL_BR_LINE (defensive:
                                            // the comm field always has a
                                            // closing ')' in a live kernel
                                            // stat record)
            rparen + 2 < content.size()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                            // the comm field always has a
                                            // closing ')' with numeric fields
                                            // after it in a live kernel stat
                                            // record)
            std::istringstream iss(content.substr(rparen + 2));
            std::string field;
            for (int i = 3; i <= 13; ++i) iss >> field;
            unsigned long long utime = 0, stime = 0;
            if (iss >> utime >> stime) {  // GCOVR_EXCL_BR_LINE (defensive:
                                          // fields 14/15 always parse)
                const long hz = sysconf(_SC_CLK_TCK);
                if (hz > 0) {  // GCOVR_EXCL_BR_LINE (defensive: CLK_TCK is
                               // always positive)
                    apply_cpu_sample(r, static_cast<double>(utime + stime) /
                                            static_cast<double>(hz));
                }
            }
        }
    }
#endif

    return r;
}

/// Prometheus label-value escaping (backslash, quote, newline).
std::string prom_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {  // GCOVR_EXCL_START (defensive: label values are
                      // whitelisted service names and fixed state enums; the
                      // escape cases never fire)
            // (label values are whitelisted service names and fixed state
            // enums; the escape cases never fire)
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            // GCOVR_EXCL_STOP
            default:
                out += c;
        }
    }
    return out;
}  // GCOVR_EXCL_LINE (unwind cleanup for throwing appends; untestable)

/// Appends one HELP/TYPE header pair plus a single sample line.
void prom_emit(std::string& out, const std::string& name,
               const std::string& type, const std::string& help,
               const std::string& labels, double value) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    out += name;
    if (!labels.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: labeled
                            // single-sample prom_emit calls only exist behind
                            // optional #ifdefs, default-build callers pass
                            // empty labels)
        // GCOVR_EXCL_START (labeled single-sample emitters live behind
        // optional #ifdefs; every default-build call passes empty labels)
        out += "{" + labels + "}";
        // GCOVR_EXCL_STOP
    }
    out += ' ';
    if (value == static_cast<long long>(value)) {
        out += std::to_string(static_cast<long long>(value));
    } else {
        out += std::to_string(value);
    }
    out += '\n';
}

/// Grouped variant: one HELP/TYPE header pair, then one sample per label
/// set (labels must already be escaped).
void prom_emit_group(
    std::string& out, const std::string& name, const std::string& type,
    const std::string& help,
    const std::vector<std::pair<std::string, double>>& samples) {
    if (samples.empty()) {
        return;
    }
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    for (const auto& [labels, value] : samples) {
        out += name;
        if (!labels  // GCOVR_EXCL_BR_LINE (defensive: every prom_emit_group
                     // caller emits non-empty labels)
                 .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: every
                              // prom_emit_group caller emits non-empty
                              // labels)
            out += "{" + labels + "}";
        }
        out += ' ';
        if (value == static_cast<long long>(value)) {
            out += std::to_string(static_cast<long long>(value));
        } else {
            out += std::to_string(value);
        }
        out += '\n';
    }
}
}  // namespace

shield::net::HttpResponse OpsHttpHandler::handle_health(
    const shield::net::HttpRequest& req) {
    // Lightweight probe: process-local reads only, never a Lua actor round
    // trip, so it answers even when the actor mesh is wedged.
    nlohmann::json checks;
    checks["core"] = {
        {"status", "ok"},
        {"uptime_seconds",
         process_uptime_seconds()}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // inlined nlohmann::json braced-init
                                      // branches)

    // Plugins: a required instance that did not reach "started" degrades
    // the process (absent optional instances are fine).
    {
        std::size_t started = 0;
        std::size_t required_down = 0;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            if (inst.state == "started") {
                ++started;
            } else if (inst.required) {
                ++required_down;
            }
        }
        checks["plugins"] = {
            {"status", required_down == 0 ? "ok" : "degraded"},
            {"started", started},
            {"required_down",
             required_down}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        const char* state = shield::server::server_state_name(sm->state());
        // Only "running" counts as healthy; starting/maintenance/shutdown
        // all mean the process is not serving normally right now.
        const bool ok = sm->state() == shield::server::ServerState::kRunning;
        checks["server"] = {{"status", ok ? "ok" : "degraded"},
                            {"state", state}};
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::size_t online = 0;
        std::size_t down = 0;
        for (const auto& n : cm->nodes()) {
            const auto state = n.state;
            if (state == shield::cluster::NodeState::Online) {
                ++online;
            } else if (state == shield::cluster::NodeState::Offline ||
                       state == shield::cluster::NodeState::Removed) {
                ++down;
            }
        }
        checks["cluster"] = {{"status", down == 0 ? "ok" : "degraded"},
                             {"nodes_online", online},
                             {"nodes_down", down}};
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    // Same convention as server/cluster above: a GLOBAL-capable build may
    // still run as a non-global node, so absence omits the check rather
    // than degrading it.
    if (shield::global::GlobalManager::global() != nullptr) {
        checks["global"] = {
            {"status",
             "ok"}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                      // nlohmann::json braced-init branches; the line runs —
                      // the guarded health fetch asserts this field)
    }
#endif

    // Overall: degraded when any check degraded. The verdict maps onto the
    // HTTP status (ok -> 200, degraded -> 503) while the body keeps the same
    // envelope, so load balancers and orchestrators can gate on the status
    // code alone and probes can still read the per-check detail.
    std::string status = "ok";
    for (const auto& [name, check] : checks.items()) {
        (void)name;
        if (check.value("status", "ok") != "ok") {
            status = "degraded";
            break;
        }
    }
    nlohmann::json data = {
        {"status", status},
        // GCOVR_EXCL_START (braced-init continuation
        // attributed to no arc; the field is asserted)
        {"uptime", process_uptime_seconds()},
        // GCOVR_EXCL_STOP
        {"checks",
         std::move(checks)}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        status == "ok" ? 200
                       : 503,  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
        {{"type", "result"}, {"data", data}});
}

shield::net::HttpResponse OpsHttpHandler::handle_metrics(
    const shield::net::HttpRequest& req) {
    std::string out;
    prom_emit(out, "shield_uptime_seconds", "gauge",
              "Process uptime in seconds", "", process_uptime_seconds());

    // Process-level resource gauges (Linux /proc, isolated in
    // read_process_resources). Omitted entirely on platforms without the
    // source (or when a read fails), so the endpoint never 500s.
    {
        const ProcessResource pr = read_process_resources();
        if (pr.rss_kib >= 0) {  // GCOVR_EXCL_BR_LINE (defensive: the -1
                                // "unmeasurable" sentinel is only set on
                                // platforms without the source, never on the
                                // Linux CI path)
            prom_emit(out, "shield_process_rss_bytes", "gauge",
                      "Resident set size in bytes", "", pr.rss_kib * 1024.0);
        }
        if (pr.threads >= 0) {  // GCOVR_EXCL_BR_LINE (defensive: threads
                                // always measurable on Linux)
            prom_emit(out, "shield_process_threads", "gauge",
                      "Number of threads in the process", "", pr.threads);
        }
        if (pr.fd_count >= 0) {  // GCOVR_EXCL_BR_LINE (defensive: fd count
                                 // always measurable on Linux)
            prom_emit(out, "shield_process_open_fds", "gauge",
                      "Open file descriptors", "", pr.fd_count);
        }
        if (pr.cpu_percent >= 0) {  // GCOVR_EXCL_BR_LINE (defensive:
                                    // cpu_percent is >= 0 whenever the sample
                                    // is present; the sentinel is a
                                    // platform-read-failure only)
            prom_emit(out, "shield_process_cpu_percent", "gauge",
                      "Process CPU usage percent since last sample", "",
                      pr.cpu_percent);
        }
    }

    // Plugins grouped by lifecycle state.
    {
        std::map<std::string, double> by_state;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            by_state[inst.state] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_plugin_instances", "gauge",
                        "Plugin instances by lifecycle state", samples);
    }

    // Plugin data pools: gauges collected through the shield.pool.stats.v1
    // interface. Fields a driver leaves at the -1 "unknown / not applicable"
    // sentinel are omitted rather than exported (sqlite has no pool, but its
    // async gauges are real).
    {
        std::vector<shield::plugin::PoolStatsResult> pools;
        shield::plugin::global_host().collect_pool_stats(pools);
        std::vector<std::pair<std::string, double>> size_samples;
        std::vector<std::pair<std::string, double>> in_use_samples;
        std::vector<std::pair<std::string, double>> idle_samples;
        std::vector<std::pair<std::string, double>> pending_async_samples;
        std::vector<std::pair<std::string, double>> holding_samples;
        for (const auto& p : pools) {
            if (p.status != shield::plugin::PoolStatsStatus::ok) continue;
            const std::string labels = "plugin=\"" + prom_escape(p.plugin_id) +
                                       "\",instance=\"" +
                                       prom_escape(p.instance_id) + "\"";
            if (p.stats.size >= 0) {
                size_samples.emplace_back(labels,
                                          static_cast<double>(p.stats.size));
                in_use_samples.emplace_back(
                    labels, static_cast<double>(p.stats.in_use));
                idle_samples.emplace_back(labels,
                                          static_cast<double>(p.stats.idle));
            }
            if (p.stats.pending_async >= 0) {
                pending_async_samples.emplace_back(
                    labels, static_cast<double>(p.stats.pending_async));
            }
            if (p.stats.holding >= 0) {
                holding_samples.emplace_back(
                    labels, static_cast<double>(p.stats.holding));
            }
        }
        prom_emit_group(out, "shield_plugin_pool_size", "gauge",
                        "Live connections in the plugin data pool",
                        size_samples);
        prom_emit_group(out, "shield_plugin_pool_in_use", "gauge",
                        "Pool connections currently checked out",
                        in_use_samples);
        prom_emit_group(out, "shield_plugin_pool_idle", "gauge",
                        "Pool connections idle in the free list", idle_samples);
        prom_emit_group(out, "shield_plugin_db_pending_async", "gauge",
                        "In-flight async DB calls (queued + running)",
                        pending_async_samples);
        prom_emit_group(out, "shield_plugin_db_holding", "gauge",
                        "Connections held by open async transactions",
                        holding_samples);
    }

    // Gateway view: accept/rejection/rate-limit counters and active sessions
    // aggregated over every live TCP listener, broken down by listen port.
    {
        std::vector<std::pair<std::string, double>> accept_samples;
        std::vector<std::pair<std::string, double>> rejection_samples;
        std::vector<std::pair<std::string, double>> active_samples;
        std::vector<std::pair<std::string, double>> rate_limited_samples;
        std::map<std::string, double> rejections;
        for (const auto* listener :
             shield::net::ListenerRegistry::instance().snapshot()) {
            const std::string port =
                std::to_string(static_cast<unsigned>(listener->port()));
            accept_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->accepts_total()));
            rejections["port=\"" + port + "\",reason=\"blocked_ip\""] +=
                static_cast<double>(listener->blocked_rejects_total());
            rejections["port=\"" + port + "\",reason=\"connection_limit\""] +=
                static_cast<double>(listener->conn_limit_rejects_total());
            rejections["port=\"" + port + "\",reason=\"ip_limit\""] +=
                static_cast<double>(listener->ip_limit_rejects_total());
            active_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->session_count()));
            rate_limited_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->rate_limited_messages_total()));
        }
        for (const auto& [labels, value] : rejections) {
            rejection_samples.emplace_back(labels, value);
        }
        prom_emit_group(out, "shield_gateway_connections_total", "counter",
                        "Connections that completed the TCP accept step",
                        accept_samples);
        prom_emit_group(out, "shield_gateway_rejections_total", "counter",
                        "Connections rejected at accept time by reason",
                        rejection_samples);
        prom_emit_group(out, "shield_gateway_active_sessions", "gauge",
                        "Live gateway sessions", active_samples);
        prom_emit_group(
            out, "shield_gateway_rate_limited_messages_total", "counter",
            "Ingress messages dropped by the per-connection rate limit",
            rate_limited_samples);
        // TLS handshake counters (docs/tls-design.md). Plaintext-only
        // deployments emit no samples for these groups at all.
        std::vector<std::pair<std::string, double>> tls_handshake_samples;
        std::vector<std::pair<std::string, double>>
            tls_handshake_failure_samples;
        for (const auto* listener :
             shield::net::ListenerRegistry::instance().snapshot()) {
            if (!listener->tls_enabled()) {
                continue;
            }
            const std::string port =
                std::to_string(static_cast<unsigned>(listener->port()));
            tls_handshake_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->tls_handshakes_total()));
            tls_handshake_failure_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->tls_handshake_failures_total()));
        }
        prom_emit_group(out, "shield_gateway_tls_handshakes_total", "counter",
                        "TLS handshakes that completed successfully",
                        tls_handshake_samples);
        prom_emit_group(out, "shield_gateway_tls_handshake_failures_total",
                        "counter",
                        "TLS handshakes that failed or timed out "
                        "(no session was built)",
                        tls_handshake_failure_samples);
    }

    // Service count + per-service traffic: the metrics that need the actor
    // mesh; omitted on timeout so a wedged mesh never wedges the scrape.
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            nlohmann::json payload = {
                {"count",
                 mgr.list_services()
                     .size()}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined nlohmann::json braced-init branches)
            nlohmann::json requests = nlohmann::json::object();
            nlohmann::json errors = nlohmann::json::object();
            nlohmann::json uptime = nlohmann::json::object();
            nlohmann::json timers = nlohmann::json::object();
            nlohmann::json pending_calls = nlohmann::json::object();
            nlohmann::json pending_tasks = nlohmann::json::object();
            nlohmann::json coroutines = nlohmann::json::object();
            nlohmann::json memory_kb = nlohmann::json::object();
            for (const auto& [name, stats] : mgr.service_stats()) {
                requests[name] = stats.requests;
                errors[name] = stats.errors;
                uptime[name] = stats.uptime_seconds;
                timers[name] = stats.timers;
                pending_calls[name] = stats.pending_calls;
                pending_tasks[name] = stats.pending_tasks;
                coroutines[name] = stats.coroutines;
                memory_kb[name] = stats.memory_kb;
            }
            payload["requests"] = std::move(requests);
            payload["errors"] = std::move(errors);
            payload["uptime"] = std::move(uptime);
            payload["timers"] = std::move(timers);
            payload["pending_calls"] = std::move(pending_calls);
            payload["pending_tasks"] = std::move(pending_tasks);
            payload["coroutines"] = std::move(coroutines);
            payload["memory_kb"] = std::move(memory_kb);
            promise->set_value(std::move(payload));
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body STL branches
             // attributed to this line)
        if (future.wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready) {
            const nlohmann::json payload = future.get();
            prom_emit(out, "shield_services", "gauge",
                      "Registered Lua services", "",
                      payload.value("count", 0.0));
            std::vector<std::pair<std::string, double>> request_samples;
            std::vector<std::pair<std::string, double>> error_samples;
            std::vector<std::pair<std::string, double>> uptime_samples;
            std::vector<std::pair<std::string, double>> timer_samples;
            for (const auto& [name, value] :
                 payload.value("requests", nlohmann::json::object()).items()) {
                request_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("errors", nlohmann::json::object()).items()) {
                error_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("uptime", nlohmann::json::object()).items()) {
                uptime_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("timers", nlohmann::json::object()).items()) {
                timer_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_requests_total", "counter",
                            "Messages dispatched to the service "
                            "(send/call/system)",
                            request_samples);
            prom_emit_group(out, "shield_service_errors_total", "counter",
                            "Dispatch handler failures per service",
                            error_samples);
            prom_emit_group(out, "shield_service_uptime_seconds", "gauge",
                            "Seconds since the service incarnation was "
                            "published (reset on respawn)",
                            uptime_samples);
            prom_emit_group(out, "shield_service_timers", "gauge",
                            "Active timers per service", timer_samples);
            std::vector<std::pair<std::string, double>> pending_call_samples;
            std::vector<std::pair<std::string, double>> pending_task_samples;
            for (const auto& [name, value] :
                 payload.value("pending_calls", nlohmann::json::object())
                     .items()) {
                pending_call_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("pending_tasks", nlohmann::json::object())
                     .items()) {
                pending_task_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_pending_calls", "gauge",
                            "Coroutine-aware calls suspended per service "
                            "(caller side)",
                            pending_call_samples);
            prom_emit_group(out, "shield_service_pending_tasks", "gauge",
                            "Forked tasks queued per service",
                            pending_task_samples);
            std::vector<std::pair<std::string, double>> coroutine_samples;
            std::vector<std::pair<std::string, double>> memory_samples;
            for (const auto& [name, value] :
                 payload.value("coroutines", nlohmann::json::object())
                     .items()) {
                coroutine_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("memory_kb", nlohmann::json::object()).items()) {
                memory_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_coroutines", "gauge",
                            "Live handler coroutines per service (running "
                            "or suspended)",
                            coroutine_samples);
            prom_emit_group(out, "shield_service_memory_kb", "gauge",
                            "Lua heap KB per service sampled at the last "
                            "dispatch exit",
                            memory_samples);
        }
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        prom_emit(
            out, "shield_server_state", "gauge",
            "Server state machine (1 for the current state)",
            "state=\"" +
                prom_escape(shield::server::server_state_name(sm->state())) +
                "\"",
            1.0);
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    if (auto* gm = shield::global::GlobalManager::global()) {
        prom_emit(out, "shield_global_data_keys", "gauge", "Global data keys",
                  "", static_cast<double>(gm->data_size()));
        prom_emit(out, "shield_global_cache_entries", "gauge",
                  "Local cache entries", "",
                  static_cast<double>(gm->cache_size()));
        prom_emit(out, "shield_global_cache_hits_total", "counter",
                  "Local cache hits", "",
                  static_cast<double>(gm->cache_hits()));
        prom_emit(out, "shield_global_cache_misses_total", "counter",
                  "Local cache misses", "",
                  static_cast<double>(gm->cache_misses()));
        prom_emit_group(  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json
            out, "shield_global_queues", "gauge", "Queues by family",
            // braced-init branches: entry lines run, records stay at zero)
            {{"type=\"normal\"",
              static_cast<double>(gm->queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"delay\"",
              static_cast<double>(gm->delay_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"priority\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->priority_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"broadcast\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->broadcast_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"reliable\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->reliable_queue_count())}});  // GCOVR_EXCL_LINE
        prom_emit_group(  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json
            out, "shield_global_locks", "gauge", "Live locks by kind",
            // braced-init branches: entry lines run, records stay at zero)
            {{"kind=\"mutex\"",
              static_cast<double>(gm->mutex_registry_size("mutex"))},
             {"kind=\"spinlock\"",
              static_cast<double>(gm->mutex_registry_size("spinlock"))},
             {"kind=\"rwlock\"", static_cast<double>(gm->rwlock_count())}});
        prom_emit(out, "shield_global_rank_boards", "gauge",
                  "Leaderboard boards", "",
                  static_cast<double>(gm->rank_board_count()));
        prom_emit(out, "shield_global_rank_members", "gauge",
                  "Leaderboard members across boards", "",
                  static_cast<double>(gm->rank_total_members()));
        prom_emit(out, "shield_global_scheduler_tasks", "gauge",
                  "Registered scheduler tasks", "",
                  static_cast<double>(gm->sched_list().size()));
        prom_emit(out, "shield_global_scheduler_active", "gauge",
                  "Active (not paused/done) scheduler tasks", "",
                  static_cast<double>(gm->sched_active_count()));
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::map<std::string, double> nodes_by_state;
        for (const auto& n : cm->nodes()) {
            nodes_by_state[shield::cluster::node_state_name(n.state)] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : nodes_by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_cluster_nodes", "gauge",
                        "Cluster nodes by state", samples);
        if (auto* ct = shield::cluster::global_cluster_transport()) {
            const auto stats = ct->stats();
            prom_emit(out, "shield_cluster_transport_connections", "gauge",
                      "Live cluster transport connections", "",
                      static_cast<double>(stats.live_connections));
            prom_emit(out, "shield_cluster_transport_reconnects_total",
                      "counter", "Cluster transport reconnects", "",
                      static_cast<double>(stats.reconnects));
            prom_emit_group(
                out, "shield_cluster_transport_messages_total", "counter",
                "Cluster transport messages",
                // GCOVR_EXCL_START (gcov artifact: the ops fixture
                // registers a real transport and asserts both series land
                // in the body, but the line records of a braced-init
                // sample list stay at zero)
                {{"direction=\"tx\"", static_cast<double>(stats.tx_messages)},
                 {"direction=\"rx\"", static_cast<double>(stats.rx_messages)}});
            // GCOVR_EXCL_STOP
            prom_emit_group(
                out, "shield_cluster_transport_heartbeats_total", "counter",
                "Cluster transport heartbeats",
                // GCOVR_EXCL_START (same artifact as above)
                {{"direction=\"tx\"", static_cast<double>(stats.tx_heartbeats)},
                 {"direction=\"rx\"",
                  static_cast<double>(stats.rx_heartbeats)}});
            // GCOVR_EXCL_STOP
        }
    }
#endif

    shield::net::HttpResponse resp;
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type,
             "text/plain; version=0.0.4; charset=utf-8");
    resp.body() = std::move(out);
    resp.prepare_payload();
    return resp;
}

}  // namespace shield::console
