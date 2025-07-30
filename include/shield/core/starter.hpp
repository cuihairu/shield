#pragma once

#include <string>
#include <vector>

namespace shield::core {

// Forward declaration
class ApplicationContext;

/**
 * Base interface for all Starter implementations.
 * A Starter is responsible for registering a set of related Services and Beans
 * with the ApplicationContext. This enables modular, self-contained plugins
 * that can be easily added or removed from the framework.
 */
class IStarter {
public:
    virtual ~IStarter() = default;

    /**
     * Initialize and register all Services and Beans provided by this Starter
     * with the given ApplicationContext.
     *
     * @param context The ApplicationContext to register components with
     */
    virtual void initialize(ApplicationContext& context) = 0;

    /**
     * Returns the name of this Starter for identification and logging purposes.
     */
    virtual std::string name() const = 0;

    /**
     * Returns a list of Starter names that this Starter depends on.
     * The framework will ensure dependency order during initialization.
     *
     * @return Vector of dependency Starter names (empty if no dependencies)
     */
    virtual std::vector<std::string> depends_on() const { return {}; }

    /**
     * Returns whether this Starter is enabled and should be initialized.
     * This allows for conditional Starter activation based on configuration
     * or runtime conditions.
     *
     * @return true if this Starter should be initialized, false otherwise
     */
    virtual bool is_enabled() const { return true; }

    /**
     * Called after all dependencies have been initialized but before
     * this Starter's initialize() method is called. Useful for any
     * pre-initialization setup or validation.
     *
     * @param context The ApplicationContext
     */
    virtual void pre_initialize(ApplicationContext& context) {}

    /**
     * Called after this Starter's initialize() method has completed
     * successfully. Useful for any post-initialization setup or validation.
     *
     * @param context The ApplicationContext
     */
    virtual void post_initialize(ApplicationContext& context) {}
};

}  // namespace shield::core