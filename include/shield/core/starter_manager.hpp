#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shield/core/starter.hpp"

namespace shield::core {

/**
 * Manages the registration and initialization of Starters.
 * Handles dependency resolution and ensures Starters are initialized
 * in the correct order.
 */
class StarterManager {
public:
    /**
     * Register a Starter with the manager.
     *
     * @param starter The Starter to register
     */
    void register_starter(std::unique_ptr<IStarter> starter);

    /**
     * Initialize all registered Starters in dependency order.
     *
     * @param context The ApplicationContext to initialize Starters with
     */
    void initialize_all(ApplicationContext& context);

    /**
     * Get the number of registered Starters.
     */
    size_t starter_count() const { return starters_.size(); }

    /**
     * Check if a Starter with the given name is registered.
     */
    bool has_starter(const std::string& name) const;

private:
    std::vector<std::unique_ptr<IStarter>> starters_;
    std::unordered_map<std::string, size_t> starter_name_to_index_;

    /**
     * Resolve the initialization order based on dependencies.
     * Returns indices of Starters in the order they should be initialized.
     */
    std::vector<size_t> resolve_initialization_order();

    /**
     * Perform topological sort on the dependency graph.
     */
    void topological_sort(size_t starter_index,
                          std::unordered_set<size_t>& visited,
                          std::unordered_set<size_t>& visiting,
                          std::vector<size_t>& order);
};

}  // namespace shield::core