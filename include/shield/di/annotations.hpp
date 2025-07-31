#pragma once

#include <memory>

namespace shield::di {

/**
 * @brief Service annotation macros for cleaner code organization
 */

// Service lifecycle annotations
#define SHIELD_SERVICE class
#define SHIELD_COMPONENT class
#define SHIELD_REPOSITORY class
#define SHIELD_CONTROLLER class

// Dependency injection annotations
#define SHIELD_INJECT(T) std::shared_ptr<T>
#define SHIELD_REQUIRED_SERVICE(T) std::shared_ptr<T>

/**
 * @brief Interface definition helper
 */
#define SHIELD_INTERFACE(name) \
    class name {               \
    public:                    \
        virtual ~name() = default;

#define SHIELD_END_INTERFACE \
    }                        \
    ;

/**
 * @brief Service implementation helper
 */
#define SHIELD_IMPLEMENTS(interface) : public interface

/**
 * @brief Convenience macro to define service dependencies using shared_ptr
 * This macro ensures dependencies are always shared_ptr types to avoid issues
 * with abstract classes
 */
#define SHIELD_DEPENDENCIES(...) \
    using dependencies = std::tuple<std::shared_ptr<__VA_ARGS__>>

/**
 * @brief Constructor injection helper - declares dependencies
 */
#define SHIELD_CONSTRUCTOR_INJECT(...) SHIELD_DEPENDENCIES(__VA_ARGS__)

}  // namespace shield::di