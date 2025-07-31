#pragma once

/**
 * @file di.hpp
 * @brief Shield Dependency Injection Framework
 *
 * A compile-time, type-safe dependency injection container for C++20.
 * Provides automatic constructor injection, service lifetime management,
 * and Spring Boot-style service registration.
 */

#include "annotations.hpp"
#include "container.hpp"
#include "service_container.hpp"

namespace shield::di {

/**
 * @brief Create a new service container
 */
inline std::unique_ptr<ServiceContainer> create_container() {
    return std::make_unique<ServiceContainer>();
}

}  // namespace shield::di