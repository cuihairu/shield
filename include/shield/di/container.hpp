#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>

namespace shield::di {

/**
 * @brief Service lifetime scope
 */
enum class ServiceLifetime {
    TRANSIENT,  // New instance every time
    SINGLETON,  // Single instance for the application
    SCOPED      // Single instance per scope (future use)
};

/**
 * @brief Service descriptor containing registration information
 */
struct ServiceDescriptor {
    std::type_index service_type;
    std::function<std::shared_ptr<void>()> factory;
    ServiceLifetime lifetime;
    std::shared_ptr<void> singleton_instance;

    ServiceDescriptor(std::type_index type,
                      std::function<std::shared_ptr<void>()> fact,
                      ServiceLifetime life)
        : service_type(type), factory(std::move(fact)), lifetime(life) {}
};

/**
 * @brief Dependency injection container
 *
 * Provides service registration and resolution with automatic dependency
 * injection. Supports singleton and transient lifetimes with compile-time type
 * safety.
 */
class Container {
private:
    std::unordered_map<std::type_index, std::unique_ptr<ServiceDescriptor>>
        services_;

public:
    Container() = default;
    ~Container() = default;

    // Non-copyable but movable
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = default;
    Container& operator=(Container&&) = default;

    /**
     * @brief Register a service with transient lifetime
     * @tparam TInterface Service interface type
     * @tparam TImplementation Service implementation type
     */
    template <typename TInterface, typename TImplementation>
    void add_transient() {
        static_assert(
            std::is_base_of_v<TInterface, TImplementation> ||
                std::is_same_v<TInterface, TImplementation>,
            "Implementation must inherit from or be the same as Interface");

        auto factory = [this]() -> std::shared_ptr<void> {
            return create_instance<TImplementation>();
        };

        register_service<TInterface>(std::move(factory),
                                     ServiceLifetime::TRANSIENT);
    }

    /**
     * @brief Register a service with singleton lifetime
     * @tparam TInterface Service interface type
     * @tparam TImplementation Service implementation type
     */
    template <typename TInterface, typename TImplementation>
    void add_singleton() {
        static_assert(
            std::is_base_of_v<TInterface, TImplementation> ||
                std::is_same_v<TInterface, TImplementation>,
            "Implementation must inherit from or be the same as Interface");

        auto factory = [this]() -> std::shared_ptr<void> {
            return create_instance<TImplementation>();
        };

        register_service<TInterface>(std::move(factory),
                                     ServiceLifetime::SINGLETON);
    }

    /**
     * @brief Register a service with a custom factory function
     * @tparam TInterface Service interface type
     * @param factory Custom factory function
     * @param lifetime Service lifetime
     */
    template <typename TInterface>
    void add_factory(std::function<std::shared_ptr<TInterface>()> factory,
                     ServiceLifetime lifetime = ServiceLifetime::TRANSIENT) {
        auto wrapper_factory =
            [factory = std::move(factory)]() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(factory());
        };

        register_service<TInterface>(std::move(wrapper_factory), lifetime);
    }

    /**
     * @brief Register an existing instance as singleton
     * @tparam TInterface Service interface type
     * @param instance Shared pointer to the instance
     */
    template <typename TInterface>
    void add_instance(std::shared_ptr<TInterface> instance) {
        auto factory = [instance]() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(instance);
        };

        auto descriptor = std::make_unique<ServiceDescriptor>(
            std::type_index(typeid(TInterface)), std::move(factory),
            ServiceLifetime::SINGLETON);

        // Pre-set the singleton instance
        descriptor->singleton_instance =
            std::static_pointer_cast<void>(instance);

        services_[std::type_index(typeid(TInterface))] = std::move(descriptor);
    }

    /**
     * @brief Resolve a service instance
     * @tparam T Service type to resolve
     * @return Shared pointer to the service instance
     * @throws std::runtime_error if service is not registered
     */
    template <typename T>
    std::shared_ptr<T> get_service() {
        auto type_index = std::type_index(typeid(T));
        auto it = services_.find(type_index);

        if (it == services_.end()) {
            throw std::runtime_error("Service not registered: " +
                                     std::string(typeid(T).name()));
        }

        auto& descriptor = it->second;

        // Handle singleton lifetime
        if (descriptor->lifetime == ServiceLifetime::SINGLETON) {
            if (!descriptor->singleton_instance) {
                descriptor->singleton_instance = descriptor->factory();
            }
            return std::static_pointer_cast<T>(descriptor->singleton_instance);
        }

        // Handle transient lifetime
        return std::static_pointer_cast<T>(descriptor->factory());
    }

    /**
     * @brief Check if a service is registered
     * @tparam T Service type to check
     * @return true if service is registered
     */
    template <typename T>
    bool is_registered() const {
        return services_.find(std::type_index(typeid(T))) != services_.end();
    }

    /**
     * @brief Get the number of registered services
     * @return Number of registered services
     */
    size_t service_count() const { return services_.size(); }

    /**
     * @brief Clear all registered services
     */
    void clear() { services_.clear(); }

private:
    /**
     * @brief Register a service with the container
     * @tparam TInterface Service interface type
     * @param factory Factory function
     * @param lifetime Service lifetime
     */
    template <typename TInterface>
    void register_service(std::function<std::shared_ptr<void>()> factory,
                          ServiceLifetime lifetime) {
        auto descriptor = std::make_unique<ServiceDescriptor>(
            std::type_index(typeid(TInterface)), std::move(factory), lifetime);

        services_[std::type_index(typeid(TInterface))] = std::move(descriptor);
    }

    /**
     * @brief Create an instance with automatic constructor injection
     * @tparam T Type to create
     * @return Shared pointer to the created instance
     */
    template <typename T>
    std::shared_ptr<void> create_instance() {
        // For now, use default constructor
        // Later we'll add automatic constructor parameter injection
        return std::static_pointer_cast<void>(std::make_shared<T>());
    }
};

}  // namespace shield::di