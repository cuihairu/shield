// [SHIELD_PLUGIN] Cross-platform shared-library loader.
//
// Self-contained dlopen/LoadLibrary wrapper used by PluginHost to map plugin
// .so/.dll/.dylib files and resolve their entry symbol. Kept self-contained
// so the shield_plugin library stays leaf-level (no dependency back into
// any host runtime module).
//
// Public header: Instance holds a PluginLibrary member, so the type must be
// visible to consumers of PluginHost.
#pragma once

#include <string>

namespace shield::plugin {

class PluginLibrary {
public:
    PluginLibrary() = default;
    ~PluginLibrary();

    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;

    PluginLibrary(PluginLibrary&& other) noexcept;
    PluginLibrary& operator=(PluginLibrary&& other) noexcept;

    // Load a shared library. On failure is_loaded() == false and `error`
    // receives a diagnostic string.
    static PluginLibrary load(const std::string& path, std::string& error);

    bool is_loaded() const;

    // Resolve an exported symbol, or NULL if not found / not loaded.
    void* resolve(const char* symbol) const;

private:
    void close();

    void* handle_ = nullptr;
};

}  // namespace shield::plugin
