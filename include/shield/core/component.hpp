#pragma once

#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace shield::core {

// Component lifecycle state
enum class ComponentState { CREATED, INITIALIZED, STARTED, STOPPED, DESTROYED };

// Component interface
class IComponent {
public:
  virtual ~IComponent() = default;
  virtual void init() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual const std::string &name() const = 0;
};

// Component base class
class Component : public IComponent {
public:
  explicit Component(const std::string &name);
  virtual ~Component() = default;

  // Implement IComponent interface
  void init() override;
  void start() override;
  void stop() override;
  const std::string &name() const override { return name_; }

  // Component state
  ComponentState state() const { return state_; }

protected:
  // Lifecycle hook methods
  virtual void on_init() {}
  virtual void on_start() {}
  virtual void on_stop() {}

private:
  std::string name_;
  ComponentState state_;
};

// Component container
class ComponentContainer {
public:
  using ComponentPtr = std::shared_ptr<IComponent>;

  template <typename T>
  void register_component(const std::shared_ptr<T> &component) {
    components_[std::type_index(typeid(T))] = component;
    components_by_name_[component->name()] = component;
  }

  template <typename T> std::shared_ptr<T> get_component() {
    auto it = components_.find(std::type_index(typeid(T)));
    if (it != components_.end()) {
      return std::dynamic_pointer_cast<T>(it->second);
    }
    return nullptr;
  }

  ComponentPtr get_component_by_name(const std::string &name) {
    auto it = components_by_name_.find(name);
    if (it != components_by_name_.end()) {
      return it->second;
    }
    return nullptr;
  }

  void init_all();
  void start_all();
  void stop_all();

private:
  std::unordered_map<std::type_index, ComponentPtr> components_;
  std::unordered_map<std::string, ComponentPtr> components_by_name_;
};

} // namespace shield::core
