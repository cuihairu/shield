# Example Plugin CMakeLists.txt
cmake_minimum_required(VERSION 3.30)

# Use same vcpkg configuration as parent project
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE
      "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "")
endif()

project(shield_example_plugin)

# Set C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Detect vcpkg triplet based on platform (same as main project)
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

# Set vcpkg paths
set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/../../vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
set(CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}" CACHE PATH "Path to vcpkg installed directory")
include_directories("${VCPKG_INSTALLED_DIR}/include")
link_directories("${VCPKG_INSTALLED_DIR}/lib")
link_directories("${VCPKG_INSTALLED_DIR}/debug/lib")

# Find required packages
find_package(Boost REQUIRED COMPONENTS system filesystem dll)

# Include Shield headers
include_directories(${CMAKE_SOURCE_DIR}/../../include)

# Create the plugin as a shared library
add_library(shield_example_plugin SHARED
    example_plugin.cpp
)

# Link with required libraries
target_link_libraries(shield_example_plugin 
    Boost::system
    Boost::filesystem
    Boost::dll
)

# Set properties for the plugin
set_target_properties(shield_example_plugin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# On Unix systems, remove the 'lib' prefix
if(UNIX)
    set_target_properties(shield_example_plugin PROPERTIES PREFIX "")
endif()

# Installation
install(TARGETS shield_example_plugin
    LIBRARY DESTINATION plugins
    RUNTIME DESTINATION plugins
)