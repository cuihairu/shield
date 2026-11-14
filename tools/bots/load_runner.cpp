// tools/bots/load_runner.cpp - ramp orchestration + per-connection workers.
#include "load_runner.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "bot_client.hpp"

namespace shield::bots {

namespace {

using Clock = std::chrono::steady_clock;

struct CumulativeRoute {
    int cum_weight = 0;
    const WorkRoute* route = nullptr;
};

/// Weighted pick via a thread-local LCG (deterministic per thread index).
const WorkRoute* pick_route(const std::vector<CumulativeRoute>& table,
                            int total_weight, std::uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    const int roll = static_cast<int>(
        seed % static_cast<std::uint32_t>(total_weight > 0 ? total_weight : 1));
    for (const auto& entry : table) {
        if (roll < entry.cum_weight) {
            return entry.route;
        }
    }
    return table.back().route;
}

void connection_worker(int index, const LoadPlan& plan, Clock::time_point start,
                       Metrics& metrics, std::atomic<int>& login_failures,
                       std::atomic<int>& connections_failed,
                       std::atomic<int>& reconnects) {
    BotClient client(plan.target);
    LoginConfig login;
    login.route_id = plan.login_route_id;
    login.reply_route_id = plan.login_reply_route_id;
    login.player_id = plan.login_player_prefix + "-" + std::to_string(index);
    const auto timeout = std::chrono::milliseconds(plan.timeout_ms);

    if (!client.connect_with_backoff(plan.reconnect_max_attempts)) {
        ++connections_failed;
        return;
    }
    if (!client.login(login, timeout, nullptr)) {
        ++login_failures;
        return;
    }

    std::vector<CumulativeRoute> table;
    int total_weight = 0;
    for (const auto& route : plan.work_routes) {
        if (route.weight > 0) {
            total_weight += route.weight;
            table.push_back({total_weight, &route});
        }
    }
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(index + 1);
    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / plan.requests_per_second));
    const auto deadline =
        start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(plan.duration_seconds));

    auto ensure_logged_in = [&]() {
        client.close();
        ++reconnects;
        if (!client.connect_with_backoff(plan.reconnect_max_attempts)) {
            return false;
        }
        return static_cast<bool>(client.login(login, timeout, nullptr));
    };

    while (Clock::now() < deadline) {
        const auto* route = pick_route(table, total_weight, seed);
        const auto send_at = Clock::now();
        auto& stats = metrics.route(route->route_id);
        if (!client.send(route->route_id, route->body.dump())) {
            if (!ensure_logged_in()) {
                ++login_failures;
                return;
            }
            continue;
        }
        if (route->reply_route_id == 0) {
            // Fire-and-forget: counted as sent, no latency to await.
            ++stats.sent;
            std::this_thread::sleep_until(send_at + interval);
            continue;
        }
        auto& reply_stats = metrics.route(route->reply_route_id);
        const auto give_up = send_at + timeout;
        bool got_reply = false;
        while (Clock::now() < give_up) {
            auto frame = client.poll_frame(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    give_up - Clock::now()));
            if (!frame) {
                if (!client.alive()) {
                    break;
                }
                continue;
            }
            if (frame->route_id == route->reply_route_id) {
                ++stats.sent;
                ++reply_stats.replies;
                reply_stats.latency_ms.push_back(
                    std::chrono::duration<double, std::milli>(Clock::now() -
                                                              send_at)
                        .count());
                got_reply = true;
                break;
            }
            // Other s2c frames (pushes, late replies) — ignored here; the
            // correlation contract is per-route, in wire order.
        }
        if (!got_reply) {
            ++stats.sent;
            ++stats.timeouts;
            if (!client.alive() && Clock::now() < deadline) {
                if (!ensure_logged_in()) {
                    ++login_failures;
                    return;
                }
            }
        }
        std::this_thread::sleep_until(send_at + interval);
    }
}

}  // namespace

LoadRunSummary run_load(const LoadPlan& plan) {
    LoadRunSummary summary;
    summary.plan_name = plan.name;
    Metrics merged;
    std::mutex merge_mutex;
    std::atomic<int> login_failures{0};
    std::atomic<int> connections_failed{0};
    std::atomic<int> reconnects{0};
    std::vector<std::thread> workers;

    const auto start = Clock::now();
    const auto end_at =
        start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(plan.duration_seconds));

    // Spawn connections along the ramp: every tick, open the gap between
    // live and target, at ~20 spawns/second to avoid a thundering herd.
    int spawned = 0;
    while (Clock::now() < end_at) {
        const auto elapsed =
            std::chrono::duration<double>(Clock::now() - start).count();
        const int target = plan.connections_at(elapsed);
        while (spawned < target && Clock::now() < end_at) {
            ++summary.connections_attempted;
            ++spawned;
            workers.emplace_back([&, index = spawned - 1] {
                // Each worker owns its metrics; the merged instance is only
                // touched under merge_mutex at worker exit.
                Metrics local;
                connection_worker(index, plan, start, local, login_failures,
                                  connections_failed, reconnects);
                std::lock_guard<std::mutex> lock(merge_mutex);
                merged.merge(local);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (Clock::now() >= end_at) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    for (auto& worker : workers) {
        worker.join();
    }

    summary.metrics = std::move(merged);
    summary.login_failures = login_failures.load();
    summary.connections_failed = connections_failed.load();
    summary.reconnects = reconnects.load();
    summary.wall_seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    return summary;
}

std::string LoadRunSummary::report() const {
    std::string out;
    char line[160];
    std::snprintf(line, sizeof(line),
                  "plan %s: %.1fs wall, connections attempted=%d failed=%d "
                  "login_fail=%d reconnects=%d\n",
                  plan_name.c_str(), wall_seconds, connections_attempted,
                  connections_failed, login_failures, reconnects);
    out += line;
    out += metrics.summary();
    return out;
}

}  // namespace shield::bots
