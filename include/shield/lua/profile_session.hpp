// [SHIELD_LUA] /ops/profile sampling session — aggregation tree & report
//
// Pure logic: no Lua state, no locks, no I/O. The sampling hook (owner
// thread, see docs/superpowers/plans/2026-09-22-ops-profile-v1.md) feeds
// add_sample; finish_report exports the JSON report handed to the HTTP
// layer via a promise.
#pragma once

#include <stddef.h>

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::lua {

/// @brief One sampled stack frame (lua_getinfo projection).
///
/// `name` is empty when Lua cannot infer it — for main-chunk and
/// tail-call frames this is the norm, not an edge case (Task 1 spike):
/// the aggregation key degrades to "?" + source:line in that case.
struct ProfileFrame {
    std::string what;    ///< "Lua" | "C" | "main"
    std::string name;    ///< function name; empty when Lua cannot infer it
    std::string source;  ///< short source path (short_src)
    int line = -1;       ///< current line; -1 when unavailable (C frames)
    bool tail = false;   ///< frame was entered via a tail call
};

/// @brief Sampling session configuration.
struct ProfileSessionConfig {
    std::string service;            ///< target service name
    uint64_t duration_ms = 5000;    ///< auto-stop after this long
    uint64_t interval = 1000000;    ///< count-hook instruction interval
    std::size_t max_depth = 64;     ///< frames retained per sample
    std::size_t max_nodes = 20000;  ///< aggregation-tree node budget
};

/// @brief Leaf-first aggregation tree for sampled stacks.
///
/// frames[0] is the currently executing frame (stack top), frames.back()
/// the stack bottom — so the tree's top level *is* the hot-frame ranking.
/// Samples sharing a call-chain prefix accumulate into shared nodes
/// (flame-graph style). All counters stay bounded: per-sample depth is
/// capped by max_depth (overflow counted in dropped_samples) and the
/// tree is capped by max_nodes (samples folded early counted in
/// truncated_frames — the sample still counts toward total_samples).
class ProfileSession {
public:
    explicit ProfileSession(ProfileSessionConfig config);

    /// @brief Accumulate one sampled stack. Owner thread only (no locks).
    void add_sample(const std::vector<ProfileFrame>& frames);

    /// @brief Freeze the session and export the full report as JSON.
    nlohmann::json finish_report(uint64_t elapsed_ms);

    const ProfileSessionConfig& config() const { return config_; }
    uint64_t total_samples() const { return total_samples_; }
    bool finished() const { return finished_; }

private:
    struct Node {
        ProfileFrame frame;
        uint64_t hits = 0;  ///< samples whose path passes through this node
        std::vector<std::unique_ptr<Node>> children;
        std::unordered_map<std::string, std::size_t> index;  ///< key -> slot
    };

    static std::string frame_key(const ProfileFrame& f);
    static nlohmann::json export_node(const Node& node, uint64_t total_samples);

    ProfileSessionConfig config_;
    std::unique_ptr<Node> root_;
    std::size_t node_count_ = 0;
    uint64_t total_samples_ = 0;
    uint64_t truncated_frames_ = 0;  ///< samples folded early: node budget
    uint64_t dropped_samples_ = 0;   ///< samples cut at max_depth
    double started_at_ = 0.0;        ///< unix seconds at construction
    bool finished_ = false;
};

}  // namespace shield::lua
