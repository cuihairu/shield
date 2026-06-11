cmake_minimum_required(VERSION 3.30)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Help vcpkg resolve the active macOS architecture deterministically when the
# repository already contains a single prepared triplet directory.
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-osx" AND
     NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/arm64-osx")
    set(CMAKE_OSX_ARCHITECTURES
        "x86_64"
        CACHE STRING "macOS architectures to build for" FORCE)
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/arm64-osx" AND
        NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-osx")
    set(CMAKE_OSX_ARCHITECTURES
        "arm64"
        CACHE STRING "macOS architectures to build for" FORCE)
  endif()
endif()

# Set default compilers based on platform if not specified
if(NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
  if(APPLE)
    set(CMAKE_C_COMPILER "/usr/bin/clang")
    set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
  elseif(UNIX AND NOT APPLE)
    set(CMAKE_C_COMPILER "/usr/bin/gcc")
    set(CMAKE_CXX_COMPILER "/usr/bin/g++")
  elseif(WIN32)
    set(CMAKE_C_COMPILER "cl")
    set(CMAKE_CXX_COMPILER "/cl")
  endif()
  message(
    STATUS
    "Using default compilers: C=${CMAKE_C_COMPILER}, CXX=${CMAKE_CXX_COMPILER}"
  )
else()
  message(
    STATUS
    "Using user-specified compilers: C=${CMAKE_C_COMPILER}, CXX=${CMAKE_CXX_COMPILER}"
  )
endif()

if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_WIN32_WINNT=0x0A00) # Windows 10
endif()

# build directory
if(NOT CMAKE_BINARY_DIR)
  set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
endif()

# vcpkg integration
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE
      "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "")
  message(STATUS "Using vcpkg toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

project(
  shield
  VERSION 0.1.0
  LANGUAGES CXX)

enable_testing()

# Build toggles
option(SHIELD_BUILD_TESTS "Build Shield tests" ON)
option(SHIELD_BUILD_EXAMPLES "Build Shield examples" ON)
option(SHIELD_ENABLE_CLUSTER "Enable cluster support (optional)" OFF)
option(SHIELD_ENABLE_GLOBAL "Enable global helpers (optional)" OFF)
option(SHIELD_ENABLE_OPS "Enable ops/diagnostics (optional)" OFF)

# Ensure config directory exists and create a minimal default config if missing.
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/config")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/config/app.yaml")
  file(WRITE "${CMAKE_SOURCE_DIR}/config/app.yaml" "---\n")
endif()

# Detect vcpkg triplet based on platform
if(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(VCPKG_TARGET_TRIPLET "x64-windows")
  else()
    set(VCPKG_TARGET_TRIPLET "x86-windows")
  endif()
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(VCPKG_TARGET_TRIPLET "arm64-osx")
  else()
    set(VCPKG_TARGET_TRIPLET "x64-osx")
  endif()
elseif(UNIX)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(VCPKG_TARGET_TRIPLET "arm64-linux")
  else()
    set(VCPKG_TARGET_TRIPLET "x64-linux")
  endif()
endif()

# Explicitly set CMAKE_PREFIX_PATH to vcpkg's installed directory
set(VCPKG_INSTALLED_DIR
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
set(CMAKE_PREFIX_PATH
    "${VCPKG_INSTALLED_DIR}"
    CACHE PATH "Path to vcpkg installed directory")

# Explicitly add vcpkg's include and lib directories to compiler search paths
include_directories("${VCPKG_INSTALLED_DIR}/include")
link_directories("${VCPKG_INSTALLED_DIR}/lib")
link_directories("${VCPKG_INSTALLED_DIR}/debug/lib")

# Enforce vcpkg-only search for includes and libraries
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# Configure version header
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/shield/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/shield/version.hpp)

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  set(GIT_COMMIT_HASH "Unknown")
endif()

add_definitions(-DGIT_COMMIT_HASH="${GIT_COMMIT_HASH}")

# Set C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Suppress deprecated-declarations warning for CAF
add_compile_options(-Wno-deprecated-declarations)

# Find required packages
find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)
find_package(CAF CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

find_path(SOL2_INCLUDE_DIRS "sol/sol.hpp")

find_package(
  Boost REQUIRED
  COMPONENTS asio
             beast
             filesystem
             log
             log_setup
             program_options
             url
             unit_test_framework
             dll)

# Try to find msgpack-cxx
find_package(msgpack-cxx CONFIG QUIET)
if(msgpack-cxx_FOUND)
  message(STATUS "Found msgpack-cxx, enabling MessagePack support")
  add_definitions(-DSHIELD_ENABLE_MESSAGEPACK)
endif()

# Try to find protobuf
find_package(Protobuf CONFIG QUIET)
if(Protobuf_FOUND)
  message(STATUS "Found protobuf, enabling Protobuf support")
  add_definitions(-DSHIELD_HAS_PROTOBUF)
  set(SHIELD_HAS_PROTOBUF TRUE)
endif()

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
file(MAKE_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})

# =============================================================================
# REFACTORED MODULE STRUCTURE
# =============================================================================

# -----------------------------------------------------------------------------
# shield_base - Base types (Result, Error, ByteBuffer, time, id)
# -----------------------------------------------------------------------------
add_library(shield_base STATIC
    src/shield_base/id.cpp
)

target_include_directories(shield_base
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

# shield_base has no external dependencies
message(STATUS "shield_base: base types module configured")

# -----------------------------------------------------------------------------
# shield_log - Logging facade and sinks
# -----------------------------------------------------------------------------
add_library(shield_log STATIC
    src/log/logger.cpp
)

target_include_directories(shield_log
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_log
    PUBLIC shield_base
    PRIVATE Boost::log Boost::log_setup
)

# -----------------------------------------------------------------------------
# shield_config - YAML loading and schema validation
# -----------------------------------------------------------------------------
add_library(shield_config STATIC
    src/config/config.cpp
)

target_include_directories(shield_config
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_config
    PUBLIC shield_base shield_log
    PRIVATE yaml-cpp::yaml-cpp
)

# -----------------------------------------------------------------------------
# shield_core - Pure core (service registry, message, timer, CAF adapter)
# DEPENDENCY CHECK: Must NOT link Lua, Asio/Beast, DB/Redis, log implementation
# -----------------------------------------------------------------------------
add_library(shield_core STATIC
    src/core/service_handle.cpp
    src/core/service_registry.cpp
    src/core/message.cpp
    src/core/caf_adapter.cpp
)

target_include_directories(shield_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

target_link_libraries(shield_core
    PUBLIC shield_base
    PRIVATE CAF::core CAF::io
)

# Dependency check - shield_core should NOT link these
if(TARGET shield_core)
    get_target_property(core_links shield_core LINK_LIBRARIES)
    message(STATUS "shield_core linked libraries: ${core_links}")
endif()

# -----------------------------------------------------------------------------
# shield_data - Raw DB/Redis access
# -----------------------------------------------------------------------------
add_library(shield_data STATIC
    src/data/data.cpp
)

target_include_directories(shield_data
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_data
    PUBLIC shield_base shield_log shield_config
    PRIVATE redis++::redis++_static hiredis::hiredis
)

# -----------------------------------------------------------------------------
# shield_transport - Frame/codec/encryption adapter
# -----------------------------------------------------------------------------
add_library(shield_transport STATIC
    src/transport/frame.cpp
    src/transport/codec.cpp
    src/transport/encryption.cpp
)

target_include_directories(shield_transport
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_transport
    PUBLIC shield_base shield_log
    PRIVATE OpenSSL::SSL OpenSSL::Crypto
)

# -----------------------------------------------------------------------------
# shield_net - Listener, session, connection management
# -----------------------------------------------------------------------------
add_library(shield_net STATIC
    src/net/session.cpp
    src/net/listener.cpp
)

target_include_directories(shield_net
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_net
    PUBLIC shield_base shield_log shield_config shield_transport
    PRIVATE Boost::asio Boost::beast
)

# -----------------------------------------------------------------------------
# shield_lua - Lua VM, API bindings, service loader
# -----------------------------------------------------------------------------
add_library(shield_lua STATIC
    src/lua/lua_service.cpp
    src/lua/lua_api.cpp
    src/lua/lua_runtime.cpp
)

target_include_directories(shield_lua
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

target_link_libraries(shield_lua
    PUBLIC shield_base shield_core
    PRIVATE shield_log shield_config shield_data
    PRIVATE ${LUA_LIBRARIES} sol2::sol2
)

target_include_directories(shield_lua
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

target_link_libraries(shield_lua
    PUBLIC shield_base shield_core
    PRIVATE shield_log shield_config shield_data shield_net
    PRIVATE ${LUA_LIBRARIES}
)

# -----------------------------------------------------------------------------
# shield_bootstrap - Starter, shield::run
# -----------------------------------------------------------------------------
add_library(shield_bootstrap STATIC
    src/bootstrap/bootstrap.cpp
    src/bootstrap/starter.cpp
)

target_include_directories(shield_bootstrap
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_bootstrap
    PUBLIC shield_base
    PRIVATE shield_log shield_config shield_core shield_lua shield_data shield_net
)

# -----------------------------------------------------------------------------
# shield - Main executable
# -----------------------------------------------------------------------------
add_executable(shield src/main.cpp)
target_link_libraries(shield PRIVATE shield_bootstrap)

# =============================================================================
# OPTIONAL MODULES
# =============================================================================

# shield_cluster (OFF by default)
if(SHIELD_ENABLE_CLUSTER)
    add_library(shield_cluster STATIC
        # TODO: Add cluster implementation
    )
    message(STATUS "shield_cluster: enabled")
endif()

# shield_global (OFF by default)
if(SHIELD_ENABLE_GLOBAL)
    add_library(shield_global STATIC
        # TODO: Add global helpers implementation
    )
    message(STATUS "shield_global: enabled")
endif()

# shield_ops (OFF by default)
if(SHIELD_ENABLE_OPS)
    add_library(shield_ops STATIC
        # TODO: Add ops implementation
    )
    message(STATUS "shield_ops: enabled")
endif()

# =============================================================================
# OPTIONAL SERIALIZATION SUPPORT
# =============================================================================

if(msgpack-cxx_FOUND)
    target_link_libraries(shield_transport PUBLIC msgpack-cxx)
    message(STATUS "MessagePack support enabled for shield_transport")
endif()

if(SHIELD_HAS_PROTOBUF)
    target_link_libraries(shield_transport PUBLIC protobuf::libprotobuf)
    message(STATUS "Protobuf support enabled for shield_transport")
endif()

# =============================================================================
# LEGACY MODULES (To be removed after migration)
# =============================================================================

# Temporarily keep legacy targets for backward compatibility
# These will be removed once migration is complete

# =============================================================================
# TESTS AND EXAMPLES
# =============================================================================

if(SHIELD_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SHIELD_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Include development tools (Git hooks, formatting, etc.)
include(cmake/GitHooks.cmake)

# =============================================================================
# BUILD SUMMARY
# =============================================================================

message(STATUS "")
message(STATUS "=== Shield Build Configuration ===")
message(STATUS "shield_base:         ENABLED")
message(STATUS "shield_log:          ENABLED")
message(STATUS "shield_config:       ENABLED")
message(STATUS "shield_core:         ENABLED (PURE)")
message(STATUS "shield_data:         ENABLED")
message(STATUS "shield_transport:    ENABLED")
message(STATUS "shield_net:           ENABLED")
message(STATUS "shield_lua:          ENABLED")
message(STATUS "shield_bootstrap:    ENABLED")
message(STATUS "")
message(STATUS "Optional modules:")
message(STATUS "  shield_cluster:    ${SHIELD_ENABLE_CLUSTER}")
message(STATUS "  shield_global:     ${SHIELD_ENABLE_GLOBAL}")
message(STATUS "  shield_ops:        ${SHIELD_ENABLE_OPS}")
message(STATUS "")
