// [SHIELD_PLUGIN] Cross-platform shared-library loader implementation.
#include "shield/plugin/plugin_library.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace shield::plugin {

PluginLibrary::~PluginLibrary() { close(); }

PluginLibrary::PluginLibrary(PluginLibrary&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

PluginLibrary& PluginLibrary::operator=(PluginLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

PluginLibrary PluginLibrary::load(const std::string& path, std::string& error) {
    PluginLibrary lib;
#ifdef _WIN32
    lib.handle_ = LoadLibraryA(path.c_str());
    if (!lib.handle_) {
        error = "LoadLibrary failed: " + std::to_string(GetLastError());
    }
#else
    // Clear any stale error, then dlopen. Capture dlerror() exactly once —
    // a second call resets it, so reading it twice would lose the message.
    dlerror();
    lib.handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle_) {
        const char* msg = dlerror();
        error = msg ? std::string(msg) : "dlopen failed";
    }
#endif
    return lib;
}

bool PluginLibrary::is_loaded() const { return handle_ != nullptr; }

void* PluginLibrary::resolve(const char* symbol) const {
    if (!handle_ || !symbol) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), symbol));
#else
    return dlsym(handle_, symbol);
#endif
}

void PluginLibrary::close() {
    if (handle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }
}

}  // namespace shield::plugin
