// [SHIELD_DATA] Cross-platform dynamic library loader implementation.
#include "dynamic_library.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

#include <cstring>
#include <string>
#include <utility>

namespace shield::data::detail {

DynamicLibrary::~DynamicLibrary() {
    if (handle_) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
    }
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        if (handle_) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

DynamicLibrary DynamicLibrary::load(const std::string& name,
                                    std::string& error) {
    DynamicLibrary lib;
#if defined(_WIN32)
    // Prepend ".dll" if not already present.
    std::string full = name;
    if (full.find('.') == std::string::npos) {
        full += ".dll";
    }
    HMODULE h = LoadLibraryA(full.c_str());
    if (!h) {
        DWORD code = GetLastError();
        error = "LoadLibraryA('" + full + "') failed (code=" +
                std::to_string(code) + ")";
        return lib;
    }
    lib.handle_ = static_cast<void*>(h);
#else
    std::string full = name;
    if (full.find('.') == std::string::npos) {
#if defined(__APPLE__)
        full = "lib" + full + ".dylib";
#else
        full = "lib" + full + ".so";
#endif
    }
    void* h = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        error = dlerror() ? dlerror() : std::string("dlopen failed");
        return lib;
    }
    lib.handle_ = h;
#endif
    return lib;
}

bool DynamicLibrary::is_loaded() const noexcept {
    return handle_ != nullptr;
}

void* DynamicLibrary::resolve(const char* symbol_name) const noexcept {
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), symbol_name));
#else
    return dlsym(handle_, symbol_name);
#endif
}

std::string host_executable_directory() {
    char buf[4096] = {};
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0) return "";
#elif defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
#endif
    std::string path(buf);
    auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

}  // namespace shield::data::detail
