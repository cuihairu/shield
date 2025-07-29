#pragma once

#include "shield/core/component.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <sol/sol.hpp>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace shield::script {

class LuaEngine : public core::Component {
public:
  explicit LuaEngine(const std::string &name);
  ~LuaEngine();

  // Register C++ function to Lua using sol2
  template <typename F>
  void register_function(const std::string &name, F &&func);

  // Register C++ class to Lua
  template <typename T>
  sol::usertype<T> register_class(const std::string &name);

  // Load and execute Lua script
  bool load_script(const std::string &filename);
  bool execute_string(const std::string &code);

  // Call Lua function with automatic type conversion
  template <typename R = void, typename... Args>
  std::conditional_t<std::is_void_v<R>, bool, std::optional<R>>
  call_function(const std::string &name, Args &&...args);

  // Get Lua state (sol2 wrapper)
  sol::state &lua() { return lua_state_; }

  // Set global variable
  template <typename T> void set_global(const std::string &name, T &&value);

  // Get global variable
  template <typename T> std::optional<T> get_global(const std::string &name);

protected:
  void on_init() override;
  void on_stop() override;

private:
  sol::state lua_state_;
  bool initialized_;
};

// Implementation of template methods using sol2
template <typename F>
void LuaEngine::register_function(const std::string &name, F &&func) {
  if (!initialized_) {
    throw std::runtime_error("LuaEngine not initialized");
  }
  lua_state_[name] = std::forward<F>(func);
}

template <typename T>
sol::usertype<T> LuaEngine::register_class(const std::string &name) {
  if (!initialized_) {
    throw std::runtime_error("LuaEngine not initialized");
  }
  return lua_state_.new_usertype<T>(name);
}

template <typename R, typename... Args>
std::conditional_t<std::is_void_v<R>, bool, std::optional<R>>
LuaEngine::call_function(const std::string &name, Args &&...args) {
  if (!initialized_) {
    if constexpr (std::is_void_v<R>) {
      return false;
    } else {
      return std::nullopt;
    }
  }

  try {
    sol::function func = lua_state_[name];
    if (!func.valid()) {
      if constexpr (std::is_void_v<R>) {
        return false;
      } else {
        return std::nullopt;
      }
    }

    if constexpr (std::is_void_v<R>) {
      func(std::forward<Args>(args)...);
      return true;
    } else {
      auto result = func(std::forward<Args>(args)...);
      if (result.valid()) {
        return result.template get<R>();
      }
    }
  } catch (const sol::error &e) {
    // Log error but don't throw
  }

  if constexpr (std::is_void_v<R>) {
    return false;
  } else {
    return std::nullopt;
  }
}

template <typename T>
void LuaEngine::set_global(const std::string &name, T &&value) {
  if (!initialized_) {
    throw std::runtime_error("LuaEngine not initialized");
  }
  lua_state_[name] = std::forward<T>(value);
}

template <typename T>
std::optional<T> LuaEngine::get_global(const std::string &name) {
  if (!initialized_) {
    return std::nullopt;
  }

  try {
    sol::object obj = lua_state_[name];
    if (obj.valid() && obj.is<T>()) {
      return obj.as<T>();
    }
  } catch (const sol::error &e) {
    // Log error
  }

  return std::nullopt;
}

} // namespace shield::script