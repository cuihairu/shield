cmake_minimum_required(VERSION 3.30)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
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
        # Windows default compiler is Visual Studio
        set(CMAKE_C_COMPILER "cl")
        set(CMAKE_CXX_COMPILER "cl")
    endif()
    message(STATUS "Using default compilers: C=${CMAKE_C_COMPILER}, CXX=${CMAKE_CXX_COMPILER}")
else()
    message(STATUS "Using user-specified compilers: C=${CMAKE_C_COMPILER}, CXX=${CMAKE_CXX_COMPILER}")
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Windows 10
endif()

# build directory
if(NOT CMAKE_BINARY_DIR)
    set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build)
endif()

# vcpkg integration
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
    message(STATUS "Using vcpkg toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

project(shield VERSION 0.1.0 LANGUAGES CXX)

enable_testing()

# Ensure config directory exists and create a dummy shield.yaml with valid YAML content
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/config")
file(WRITE "${CMAKE_SOURCE_DIR}/config/shield.yaml" "---\n")
message(STATUS "Ensured dummy config file exists and is valid: ${CMAKE_SOURCE_DIR}/config/shield.yaml")

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
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

message(STATUS "Detected vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

# Explicitly set CMAKE_PREFIX_PATH to vcpkg's installed directory
set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
set(CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}" CACHE PATH "Path to vcpkg installed directory")

# Explicitly add vcpkg's include and lib directories to compiler search paths
include_directories("${VCPKG_INSTALLED_DIR}/include")
link_directories("${VCPKG_INSTALLED_DIR}/lib")
link_directories("${VCPKG_INSTALLED_DIR}/debug/lib") # For debug libraries

# Enforce vcpkg-only search for includes and libraries
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# Enforce vcpkg-only search for includes and libraries
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# Configure version header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/shield/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/shield/version.hpp
)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
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
find_package(Protobuf CONFIG REQUIRED)

find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)
find_package(etcd-cpp-api CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(CAF CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Try to find prometheus-cpp, make it optional for now
find_package(prometheus-cpp CONFIG QUIET)
if(prometheus-cpp_FOUND)
    message(STATUS "Found prometheus-cpp, enabling Prometheus support")
    add_definitions(-DSHIELD_ENABLE_PROMETHEUS)
    
    # Check what targets are available
    if(TARGET prometheus-cpp::core)
        set(PROMETHEUS_CORE_TARGET "prometheus-cpp::core")
    elseif(TARGET prometheus::core)
        set(PROMETHEUS_CORE_TARGET "prometheus::core")
    else()
        message(WARNING "prometheus-cpp core target not found")
        set(prometheus-cpp_FOUND FALSE)
    endif()
    
    if(TARGET prometheus-cpp::gateway)
        set(PROMETHEUS_GATEWAY_TARGET "prometheus-cpp::gateway")
    elseif(TARGET prometheus::gateway)
        set(PROMETHEUS_GATEWAY_TARGET "prometheus::gateway")
    else()
        message(WARNING "prometheus-cpp gateway target not found")
        set(PROMETHEUS_GATEWAY_TARGET "")
    endif()
else()
    message(STATUS "prometheus-cpp not found, Prometheus support disabled")
endif()

find_package(Boost REQUIRED COMPONENTS asio beast log log_setup program_options url unit_test_framework) # <--- ADDED
find_path(SOL2_INCLUDE_DIRS "sol/sol.hpp")

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Create output directories if they don't exist
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
file(MAKE_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})

# Find all source files
file(GLOB_RECURSE CORE_SOURCES
    "src/*.cpp"
    "src/discovery/*.cpp"
    "src/discovery/service_instance.cpp"
    "src/discovery/local_discovery.cpp"
    "src/discovery/redis_discovery.cpp"
    "src/discovery/nacos_discovery.cpp"
    "src/discovery/consul_discovery.cpp"
    "src/actor/*.cpp"
    "src/net/*.cpp"
    "src/protocol/*.cpp"
    "src/serialization/*.cpp"
    "src/metrics/*.cpp"
    "src/caf_initializer.cpp"
)
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp") # Explicitly exclude main.cpp

# Print found source files (optional, for debugging)
message(STATUS "Found core library source files:")
foreach(SOURCE ${CORE_SOURCES})
    message(STATUS "  ${SOURCE}")
endforeach()

# Create the core static library
add_library(shield_core STATIC ${CORE_SOURCES})
target_include_directories(shield_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include  # Directory for generated headers
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)
target_link_libraries(shield_core
    PUBLIC
        ${LUA_LIBRARIES}
        Boost::asio
        Boost::beast
        yaml-cpp::yaml-cpp
        Boost::log
        Boost::log_setup
        Boost::program_options
        Boost::url
        CAF::core
        CAF::io
        nlohmann_json::nlohmann_json
        
        
        etcd-cpp-api
        redis++::redis++_static
        Threads::Threads
        gRPC::gpr
        gRPC::grpc
        gRPC::grpc++
        gRPC::grpc++_alts
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Add prometheus-cpp if available
if(prometheus-cpp_FOUND)
    target_link_libraries(shield_core
        PUBLIC
            ${PROMETHEUS_CORE_TARGET}
    )
    if(PROMETHEUS_GATEWAY_TARGET)
        target_link_libraries(shield_core
            PUBLIC
                ${PROMETHEUS_GATEWAY_TARGET}
        )
    endif()
endif()

add_executable(shield src/main.cpp)
target_link_libraries(shield PRIVATE shield_core)

# Add examples directory
add_subdirectory(examples)
add_subdirectory(tests)