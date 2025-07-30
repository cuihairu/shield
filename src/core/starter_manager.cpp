#include "shield/core/starter_manager.hpp"

#include <algorithm>
#include <stdexcept>

#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"

namespace shield::core {

void StarterManager::register_starter(std::unique_ptr<IStarter> starter) {
    if (!starter) {
        throw std::invalid_argument("Cannot register null Starter");
    }

    const std::string& name = starter->name();
    if (has_starter(name)) {
        throw std::runtime_error("Starter with name '" + name +
                                 "' already registered");
    }

    size_t index = starters_.size();
    starter_name_to_index_[name] = index;
    starters_.push_back(std::move(starter));

    SHIELD_LOG_DEBUG << "Registered Starter: " << name;
}

bool StarterManager::has_starter(const std::string& name) const {
    return starter_name_to_index_.find(name) != starter_name_to_index_.end();
}

void StarterManager::initialize_all(ApplicationContext& context) {
    if (starters_.empty()) {
        SHIELD_LOG_INFO << "No Starters registered";
        return;
    }

    // Filter enabled Starters
    std::vector<size_t> enabled_starters;
    for (size_t i = 0; i < starters_.size(); ++i) {
        if (starters_[i]->is_enabled()) {
            enabled_starters.push_back(i);
        } else {
            SHIELD_LOG_INFO << "Skipping disabled Starter: "
                            << starters_[i]->name();
        }
    }

    if (enabled_starters.empty()) {
        SHIELD_LOG_INFO << "No enabled Starters to initialize";
        return;
    }

    // Resolve initialization order
    std::vector<size_t> initialization_order = resolve_initialization_order();

    // Filter the order to only include enabled Starters
    std::vector<size_t> enabled_order;
    for (size_t index : initialization_order) {
        if (std::find(enabled_starters.begin(), enabled_starters.end(),
                      index) != enabled_starters.end()) {
            enabled_order.push_back(index);
        }
    }

    SHIELD_LOG_INFO << "Initializing " << enabled_order.size() << " Starters";

    // Initialize Starters in order
    for (size_t index : enabled_order) {
        auto& starter = starters_[index];

        try {
            SHIELD_LOG_INFO << "Pre-initializing Starter: " << starter->name();
            starter->pre_initialize(context);

            SHIELD_LOG_INFO << "Initializing Starter: " << starter->name();
            starter->initialize(context);

            SHIELD_LOG_INFO << "Post-initializing Starter: " << starter->name();
            starter->post_initialize(context);

            SHIELD_LOG_INFO << "Successfully initialized Starter: "
                            << starter->name();
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to initialize Starter '"
                             << starter->name() << "': " << e.what();
            throw;
        }
    }

    SHIELD_LOG_INFO << "All Starters initialized successfully";
}

std::vector<size_t> StarterManager::resolve_initialization_order() {
    std::vector<size_t> order;
    std::unordered_set<size_t> visited;
    std::unordered_set<size_t> visiting;

    // Perform topological sort
    for (size_t i = 0; i < starters_.size(); ++i) {
        if (visited.find(i) == visited.end()) {
            topological_sort(i, visited, visiting, order);
        }
    }

    // Reverse to get correct dependency order (dependencies first)
    std::reverse(order.begin(), order.end());
    return order;
}

void StarterManager::topological_sort(size_t starter_index,
                                      std::unordered_set<size_t>& visited,
                                      std::unordered_set<size_t>& visiting,
                                      std::vector<size_t>& order) {
    if (visiting.find(starter_index) != visiting.end()) {
        throw std::runtime_error(
            "Circular dependency detected involving Starter: " +
            starters_[starter_index]->name());
    }

    if (visited.find(starter_index) != visited.end()) {
        return;
    }

    visiting.insert(starter_index);

    // Process dependencies first
    const auto& dependencies = starters_[starter_index]->depends_on();
    for (const std::string& dep_name : dependencies) {
        auto it = starter_name_to_index_.find(dep_name);
        if (it == starter_name_to_index_.end()) {
            throw std::runtime_error(
                "Starter '" + starters_[starter_index]->name() +
                "' depends on unknown Starter: " + dep_name);
        }

        size_t dep_index = it->second;
        topological_sort(dep_index, visited, visiting, order);
    }

    visiting.erase(starter_index);
    visited.insert(starter_index);
    order.push_back(starter_index);
}

}  // namespace shield::core