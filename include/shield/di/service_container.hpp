#pragma once

#include <tuple>
#include <utility>

#include "container.hpp"

namespace shield::di {

/**
 * @brief Type trait to detect if a type has a dependencies typedef
 */
template <typename T, typename = void>
struct has_dependencies : std::false_type {};

template <typename T>
struct has_dependencies<T, std::void_t<typename T::dependencies>>
    : std::true_type {};

template <typename T>
constexpr bool has_dependencies_v = has_dependencies<T>::value;

/**
 * @brief Extract shared_ptr types from tuple of dependency types
 */
template <typename Tuple>
struct make_shared_ptr_tuple;

template <typename... Types>
struct make_shared_ptr_tuple<std::tuple<Types...>> {
    using type = std::tuple<std::shared_ptr<Types>...>;
};

template <typename Tuple>
using make_shared_ptr_tuple_t = typename make_shared_ptr_tuple<Tuple>::type;

/**
 * @brief Helper to create instances with automatic dependency injection
 */
template <typename T>
class InstanceFactory {
public:
    /**
     * @brief Create instance with dependencies if they exist
     */
    static std::shared_ptr<T> create(Container& container) {
        if constexpr (has_dependencies_v<T>) {
            return create_with_dependencies(container,
                                            typename T::dependencies{});
        } else {
            return std::make_shared<T>();
        }
    }

private:
    /**
     * @brief Create instance with dependency tuple of shared_ptr types
     */
    template <typename Dep>
    static std::shared_ptr<T> create_with_dependencies(
        Container& container, std::tuple<std::shared_ptr<Dep>>) {
        return std::make_shared<T>(container.get_service<Dep>());
    }
};

/**
 * @brief Extended container with automatic constructor injection
 */
class ServiceContainer : public Container {
public:
    /**
     * @brief Register service with automatic constructor injection
     * @tparam TInterface Service interface type
     * @tparam TImplementation Service implementation type
     */
    template <typename TInterface, typename TImplementation>
    void add_transient_auto() {
        static_assert(
            std::is_base_of_v<TInterface, TImplementation> ||
                std::is_same_v<TInterface, TImplementation>,
            "Implementation must inherit from or be the same as Interface");

        auto factory = [this]() -> std::shared_ptr<TInterface> {
            return InstanceFactory<TImplementation>::create(*this);
        };

        Container::add_factory<TInterface>(factory, ServiceLifetime::TRANSIENT);
    }

    /**
     * @brief Register singleton service with automatic constructor injection
     * @tparam TInterface Service interface type
     * @tparam TImplementation Service implementation type
     */
    template <typename TInterface, typename TImplementation>
    void add_singleton_auto() {
        static_assert(
            std::is_base_of_v<TInterface, TImplementation> ||
                std::is_same_v<TInterface, TImplementation>,
            "Implementation must inherit from or be the same as Interface");

        auto factory = [this]() -> std::shared_ptr<TInterface> {
            return InstanceFactory<TImplementation>::create(*this);
        };

        Container::add_factory<TInterface>(factory, ServiceLifetime::SINGLETON);
    }
};

}  // namespace shield::di

/**
 * @brief Service registration macros for cleaner syntax
 */
#define SHIELD_REGISTER_TRANSIENT(container, interface, impl) \
    (container).add_transient_auto<interface, impl>()

#define SHIELD_REGISTER_SINGLETON(container, interface, impl) \
    (container).add_singleton_auto<interface, impl>()

#define SHIELD_REGISTER_INSTANCE(container, interface, instance) \
    (container).add_instance<interface>(instance)