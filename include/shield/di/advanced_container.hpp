#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shield/di/container.hpp"

namespace shield::di {

/**
 * @brief Dependency resolution context for circular dependency detection
 */
class ResolutionContext {
public:
    void push_resolution(std::type_index type) {
        if (resolution_stack_.count(type)) {
            throw std::runtime_error("Circular dependency detected for type: " +
                                     std::string(type.name()));
        }
        resolution_stack_.insert(type);
        resolution_order_.push_back(type);
    }

    void pop_resolution(std::type_index type) {
        resolution_stack_.erase(type);
        if (!resolution_order_.empty() && resolution_order_.back() == type) {
            resolution_order_.pop_back();
        }
    }

    bool is_resolving(std::type_index type) const {
        return resolution_stack_.count(type) > 0;
    }

    std::vector<std::string> get_resolution_chain() const {
        std::vector<std::string> chain;
        for (const auto& type : resolution_order_) {
            chain.push_back(type.name());
        }
        return chain;
    }

private:
    std::unordered_set<std::type_index> resolution_stack_;
    std::vector<std::type_index> resolution_order_;
};

/**
 * @brief RAII guard for dependency resolution tracking
 */
class ResolutionGuard {
public:
    ResolutionGuard(ResolutionContext& ctx, std::type_index type)
        : context_(ctx), type_(type) {
        context_.push_resolution(type_);
    }

    ~ResolutionGuard() { context_.pop_resolution(type_); }

private:
    ResolutionContext& context_;
    std::type_index type_;
};

/**
 * @brief Service factory interface for advanced dependency injection
 */
class IServiceFactory {
public:
    virtual ~IServiceFactory() = default;
    virtual std::shared_ptr<void> create(Container& container,
                                         ResolutionContext& context) = 0;
    virtual std::type_index get_service_type() const = 0;
    virtual ServiceLifetime get_lifetime() const = 0;
};

/**
 * @brief Template factory for automatic constructor injection
 */
template <typename T>
class AutoInjectFactory : public IServiceFactory {
public:
    std::shared_ptr<void> create(Container& container,
                                 ResolutionContext& context) override {
        ResolutionGuard guard(context, std::type_index(typeid(T)));

        if constexpr (std::is_constructible_v<T>) {
            // Default constructor
            return std::static_pointer_cast<void>(std::make_shared<T>());
        } else {
            return create_with_injection(container, context);
        }
    }

    std::type_index get_service_type() const override {
        return std::type_index(typeid(T));
    }

    ServiceLifetime get_lifetime() const override {
        return ServiceLifetime::SINGLETON;  // Default to singleton
    }

private:
    template <typename U = T>
    std::shared_ptr<void> create_with_injection(Container& container,
                                                ResolutionContext& context) {
        // This would need C++20 concepts or traits to detect constructor
        // parameters For now, we'll use a simplified approach with explicit
        // registration
        static_assert(
            std::is_constructible_v<U>,
            "Type must be constructible or have explicit injection factory");
        return std::static_pointer_cast<void>(std::make_shared<U>());
    }
};

/**
 * @brief Advanced dependency injection container with constructor injection
 */
class AdvancedContainer : public Container {
public:
    AdvancedContainer() = default;
    ~AdvancedContainer() = default;

    /**
     * @brief Register service with automatic constructor injection
     */
    template <typename TInterface, typename TImplementation = TInterface>
    void register_auto_inject(
        ServiceLifetime lifetime = ServiceLifetime::SINGLETON) {
        static_assert(
            std::is_base_of_v<TInterface, TImplementation> ||
                std::is_same_v<TInterface, TImplementation>,
            "Implementation must inherit from or be the same as Interface");

        auto factory = std::make_unique<AutoInjectFactory<TImplementation>>();
        factories_[std::type_index(typeid(TInterface))] = std::move(factory);
    }

    /**
     * @brief Register service with custom injection factory
     */
    template <typename TInterface>
    void register_factory_advanced(
        std::function<std::shared_ptr<TInterface>(AdvancedContainer&)> factory,
        ServiceLifetime lifetime = ServiceLifetime::SINGLETON) {
        auto wrapper_factory = std::make_unique<CustomFactory<TInterface>>(
            std::move(factory), lifetime);
        factories_[std::type_index(typeid(TInterface))] =
            std::move(wrapper_factory);
    }

    /**
     * @brief Resolve service with circular dependency detection
     */
    template <typename T>
    std::shared_ptr<T> resolve() {
        ResolutionContext context;
        return resolve_with_context<T>(context);
    }

    /**
     * @brief Try resolve service (returns nullptr if not registered)
     */
    template <typename T>
    std::shared_ptr<T> try_resolve() {
        try {
            return resolve<T>();
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    /**
     * @brief Check if service can be resolved
     */
    template <typename T>
    bool can_resolve() const {
        auto type_index = std::type_index(typeid(T));
        return factories_.find(type_index) != factories_.end() ||
               Container::is_registered<T>();
    }

private:
    template <typename T>
    std::shared_ptr<T> resolve_with_context(ResolutionContext& context) {
        auto type_index = std::type_index(typeid(T));

        // First check if already resolved as singleton
        if (auto it = singletons_.find(type_index); it != singletons_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }

        // Check if we have a factory
        if (auto factory_it = factories_.find(type_index);
            factory_it != factories_.end()) {
            auto instance = factory_it->second->create(*this, context);

            // Cache singleton instances
            if (factory_it->second->get_lifetime() ==
                ServiceLifetime::SINGLETON) {
                singletons_[type_index] = instance;
            }

            return std::static_pointer_cast<T>(instance);
        }

        // Fallback to base container
        return Container::get_service<T>();
    }

    template <typename T>
    class CustomFactory : public IServiceFactory {
    public:
        CustomFactory(
            std::function<std::shared_ptr<T>(AdvancedContainer&)> factory,
            ServiceLifetime lifetime)
            : factory_(std::move(factory)), lifetime_(lifetime) {}

        std::shared_ptr<void> create(Container& container,
                                     ResolutionContext& context) override {
            auto& advanced_container =
                static_cast<AdvancedContainer&>(container);
            return std::static_pointer_cast<void>(factory_(advanced_container));
        }

        std::type_index get_service_type() const override {
            return std::type_index(typeid(T));
        }

        ServiceLifetime get_lifetime() const override { return lifetime_; }

    private:
        std::function<std::shared_ptr<T>(AdvancedContainer&)> factory_;
        ServiceLifetime lifetime_;
    };

private:
    std::unordered_map<std::type_index, std::unique_ptr<IServiceFactory>>
        factories_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> singletons_;
};

/**
 * @brief Dependency injection helper macros for constructor injection
 */
#define SHIELD_INJECT_CONSTRUCTOR(ClassName, ...)        \
    static std::shared_ptr<ClassName> create_injected(   \
        shield::di::AdvancedContainer& container) {      \
        return std::make_shared<ClassName>(__VA_ARGS__); \
    }

/**
 * @brief Service registration helper
 */
template <typename TInterface, typename TImplementation = TInterface>
void register_service_advanced(AdvancedContainer& container) {
    if constexpr (requires { TImplementation::create_injected(container); }) {
        // Has custom injection factory
        container.register_factory_advanced<TInterface>(
            [](AdvancedContainer& c) {
                return TImplementation::create_injected(c);
            });
    } else {
        // Use auto injection
        container.register_auto_inject<TInterface, TImplementation>();
    }
}

}  // namespace shield::di