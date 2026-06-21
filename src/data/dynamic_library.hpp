// [SHIELD_DATA] Cross-platform dynamic library loader used by the
// database plugin system. Tiny wrapper around LoadLibrary / dlopen
// so data.cpp stays platform-agnostic.
#pragma once

#include <string>

namespace shield::data::detail {

// Opaque handle to a loaded dynamic module.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    // Load a module by name. The driver suffix (.dll/.so/.dylib) and
    // any platform-specific prefix (lib) are appended automatically.
    // Returns empty (is_loaded() == false) on failure; error() has
    // the platform message.
    static DynamicLibrary load(const std::string& name,
                               std::string& error);

    bool is_loaded() const noexcept;

    // Resolve an exported symbol. Returns nullptr on failure.
    void* resolve(const char* symbol_name) const noexcept;

private:
    void* handle_ = nullptr;
};

// Returns the directory containing the running executable, with a
// trailing path separator. Empty string on failure (rare).
// Used by the DB plugin loader to find shield_db_*.dll alongside
// the host binary without requiring PATH manipulation.
std::string host_executable_directory();

}  // namespace shield::data::detail
