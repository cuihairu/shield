#include "shield/lua/profile_session.hpp"

#include <algorithm>
#include <chrono>

namespace shield::lua {

ProfileSession::ProfileSession(ProfileSessionConfig config)
    : config_(std::move(config)),
      root_(std::make_unique<Node>()),
      started_at_(std::chrono::duration<double>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()) {}

std::string ProfileSession::frame_key(const ProfileFrame& f) {
    // name-degraded frames (main chunk, tail calls) aggregate under "?" —
    // source:line still disambiguates them.
    return f.what + "|" + (f.name.empty() ? "?" : f.name) + "|" + f.source +
           ":" + std::to_string(f.line);
}

// Recursive export: sort children by hits (desc) and emit the bounded
// subtree. Depth is already bounded by the sampler's max_depth, so the
// recursion cannot run away.
nlohmann::json ProfileSession::export_node(const Node& node,
                                           uint64_t total_samples) {
    nlohmann::json out = {
        {"what", node.frame.what},
        {"name", node.frame.name.empty() ? "?" : node.frame.name},
        {"source", node.frame.source},
        {"line", node.frame.line},
        {"tail", node.frame.tail},
        {"hits", node.hits},
        {"pct", total_samples == 0 ? 0.0
                                   : static_cast<double>(node.hits) * 100.0 /
                                         static_cast<double>(total_samples)},
        {"children", nlohmann::json::array()}};
    std::vector<const Node*> kids;
    kids.reserve(node.children.size());
    for (const auto& c : node.children) {
        kids.push_back(c.get());
    }
    std::stable_sort(
        kids.begin(), kids.end(),
        [](const Node* a, const Node* b) { return a->hits > b->hits; });
    for (const auto* kid : kids) {
        out["children"].push_back(export_node(*kid, total_samples));
    }
    return out;
}

void ProfileSession::add_sample(const std::vector<ProfileFrame>& frames) {
    if (finished_) {
        return;
    }
    ++total_samples_;

    const std::size_t depth = std::min(frames.size(), config_.max_depth);
    if (frames.size() > config_.max_depth) {
        ++dropped_samples_;
    }

    Node* node = root_.get();
    for (std::size_t i = 0; i < depth; ++i) {
        const ProfileFrame& f = frames[i];
        const std::string key = frame_key(f);
        auto it = node->index.find(key);
        Node* next = nullptr;
        if (it != node->index.end()) {
            next = node->children[it->second].get();
        } else {
            if (node_count_ >= config_.max_nodes) {
                // Budget exhausted: keep the sample's total count but fold
                // the remaining frames into this node.
                ++truncated_frames_;
                break;
            }
            auto owned = std::make_unique<Node>();
            owned->frame = f;
            node->index.emplace(key, node->children.size());
            next = owned.get();
            node->children.push_back(std::move(owned));
            ++node_count_;
        }
        ++next->hits;
        node = next;
    }
}

nlohmann::json ProfileSession::finish_report(uint64_t elapsed_ms) {
    finished_ = true;
    const double stopped_at =
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    nlohmann::json frames = nlohmann::json::array();
    std::vector<const Node*> kids;
    kids.reserve(root_->children.size());
    for (const auto& c : root_->children) {
        kids.push_back(c.get());
    }
    std::stable_sort(
        kids.begin(), kids.end(),
        [](const Node* a, const Node* b) { return a->hits > b->hits; });
    for (const auto* kid : kids) {
        frames.push_back(export_node(*kid, total_samples_));
    }

    return {{"service", config_.service},
            {"duration_ms", config_.duration_ms},
            {"interval", config_.interval},
            {"started_at", started_at_},
            {"stopped_at", stopped_at},
            {"elapsed_ms", elapsed_ms},
            {"total_samples", total_samples_},
            {"truncated_frames", truncated_frames_},
            {"dropped_samples", dropped_samples_},
            {"frames", std::move(frames)}};
}

}  // namespace shield::lua
