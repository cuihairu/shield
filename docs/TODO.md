
# Shield Framework Overhaul: TODO & Architecture Plan

This document outlines the architectural vision and phased implementation plan for refactoring the Shield project. The goal is to evolve it from its current state into a modern, modular, and extensible C++ framework, heavily inspired by the robust design principles of Spring Boot.

---

## 1. High-Level Vision & Guiding Principles

Our objective is to build a framework, not just an application. This means prioritizing:

- **Modularity & Extensibility**: New features (e.g., database access, new protocols) should be addable as self-contained plugins (`Starters`) without modifying the core.
- **Inversion of Control (IoC)**: The framework manages the lifecycle and dependencies of components, not the components themselves.
- **Dependency Injection (DI)**: Components receive their dependencies from the framework, making them easier to test and reason about.
- **Convention over Configuration**: The framework should make sensible defaults, reducing boilerplate code.
- **Clear Separation of Concerns**: Each part of the framework has a single, well-defined responsibility.

---

## 2. Core Concepts & Terminology

To ensure clarity, we will adopt the following terminology:

- **`ApplicationContext`**: The heart of the framework. An IoC container that manages the lifecycle of all Beans and Services. It is responsible for instantiation, assembly (DI), and lifecycle management (`init`, `start`, `stop`). This replaces the old `ComponentManager`.

- **`Service`**: A major, stateful, long-lived object that provides a core business capability. Examples: `GatewayService`, `MetricsService`. Services have a defined lifecycle. This replaces the old `Component`.

- **`ReloadableService`**: A specialized `Service` that can react to configuration changes at runtime.

- **`Bean`**: Any object managed by the `ApplicationContext`. This is a finer-grained concept than a `Service`. A `Service` is a `Bean`, but a `Bean` can also be a simple utility class, a factory, or a configuration object.

- **`Config`**: A specialized `Bean` whose sole purpose is to hold configuration data. It is populated by the `ConfigManager`.

- **`Starter`**: A self-contained plugin that registers a set of related Beans and Services with the `ApplicationContext`. For example, a `LuaStarter` would register everything needed for Lua scripting.

---

## 3. Detailed Architecture Blueprint

### 3.1. `ConfigManager`: The Data & Notification Hub

- **Responsibility**: Manages configuration data ONLY.
- **Key Features**:
    1.  **Loading**: Reads configuration from files (`.yaml`, `.json`, etc.) into an internal `boost::property_tree::ptree`.
    2.  **Hot Reloading**: Implements a transactional "Clone-Validate-Swap" mechanism to safely apply configuration changes at runtime.
    3.  **Notifications**: Provides a type-safe publish/subscribe system (`subscribe_to_reloads<T>`) for other parts of the system to listen for configuration changes.
- **Crucial Constraint**: The `ConfigManager` has **zero knowledge** of `Services`, `ApplicationContext`, or any other business logic component. It only deals with data and callbacks.

### 3.2. The `Config` Inheritance Hierarchy

To achieve type-safety and clear intent for hot-reloading:

1.  `ComponentConfig` (to be renamed `BaseConfig` or similar):
    - The abstract base for all configuration beans.
    - `virtual from_ptree(...) = 0`
    - `virtual supports_hot_reload() const { return false; }`

2.  `ClonableComponentConfig<Derived>`:
    - Inherits from `BaseConfig`.
    - Uses CRTP to provide a concrete `clone()` implementation.
    - Does **not** override `supports_hot_reload()`.

3.  `ReloadableComponentConfig<Derived>`:
    - Inherits from `ClonableComponentConfig<Derived>`.
    - Its sole purpose is to `override supports_hot_reload() const { return true; }`.
    - This provides a clear, self-documenting way to mark a `Config` as reloadable.

### 3.3. `ApplicationContext`: The Core IoC Container

- **Responsibility**: Manages object lifecycle and dependency injection.
- **Key Features**:
    1.  **Bean/Service Registry**: Contains a central repository for all managed objects, likely using `std::unordered_map<std::string, boost::any>` to store named beans.
    2.  **Lifecycle Management**: Calls the lifecycle hooks (`on_init`, `on_start`, `on_stop`) for all registered `Services` in the correct order.
    3.  **The "Glue"**: It subscribes to the `ConfigManager`'s reload events and dispatches them to the appropriate `ReloadableService` instances.

### 3.4. `Service` & `ReloadableService`

- **`Service` Interface**:
    - `virtual void on_init(ApplicationContext& ctx) {}`
    - `virtual void on_start() {}`
    - `virtual void on_stop() {}`
- **`ReloadableService` Interface**:
    - Inherits from `Service`.
    - `virtual void on_config_reloaded() = 0;`

---

## 4. Phased Implementation Plan (The TODO List)

### ☐ Phase 1: Configuration System Refactoring (The Foundation)

*This phase focuses on making the configuration system robust and ready for hot reloading, without yet touching the component/service model.*

- [ ] **1.1. Clean up `ComponentConfig`**: Remove the `to_yaml` method from the base class and all derived classes.
- [ ] **1.2. Implement New `Config` Hierarchy**:
    - [ ] In `shield/config/config.hpp`, define the `ClonableComponentConfig<Derived>` template class.
    - [ ] In the same file, define the `ReloadableComponentConfig<Derived>` template class.
- [ ] **1.3. Refactor Existing Config Classes**:
    - [ ] Change the base class of all existing config objects (e.g., `GatewayConfig`, `LogConfig`) to `ReloadableComponentConfig<T>`.
    - [ ] Remove any manual `clone()` or `supports_hot_reload()` implementations from them.
- [ ] **1.4. Implement Transactional Hot Reload**: In `ConfigManager::reload_config`, implement the "Clone-Validate-Swap" logic.
- [ ] **1.5. Implement Callback Mechanism**: In `ConfigManager`, implement the `subscribe_to_reloads<T>` method, including the `static_assert` to ensure it only accepts `ReloadableComponentConfig` derivatives.

### ☐ Phase 2: Introduce `ApplicationContext` & `Service` Lifecycle

*This phase rebrands our components into services and introduces the central lifecycle manager.*

- [ ] **2.1. Rename Core Concepts**:
    - [ ] Rename `Component` to `Service`.
    - [ ] Rename `ReloadableComponent` to `ReloadableService`.
    - [ ] Rename `ComponentManager` to `ApplicationContext`.
- [ ] **2.2. Implement Basic `ApplicationContext`**: Create the `ApplicationContext` class with a registry for `Service` instances. It should manage them in an ordered list to respect startup/shutdown order.
- [ ] **2.3. Implement Service Lifecycle**: Implement the `init_all()`, `start_all()`, and `stop_all()` methods in `ApplicationContext` that call the corresponding `on_init`, `on_start`, `on_stop` hooks on the registered services.
- [ ] **2.4. Integrate `ApplicationContext` with `ConfigManager`**:
    - [ ] The `ApplicationContext` will be responsible for subscribing to the `ConfigManager`.
    - [ ] When registering a `ReloadableService`, the `ApplicationContext` should automatically create a callback that, when triggered, calls the service's `on_config_reloaded()` method.
- [ ] **2.5. Refactor `main()` / `server_command.cpp`**: The application entry point should now primarily interact with the `ApplicationContext` to register services and start the application.

### ☐ Phase 3: Implement Basic Dependency Injection (DI)

*This phase makes the framework truly powerful by decoupling services from each other.*

- [ ] **3.1. Evolve `ApplicationContext` to a Bean Container**: Enhance the internal registry to use `boost::any` to store any type of named `Bean`, not just `Services`.
- [ ] **3.2. Implement Constructor Injection (Manual Resolution)**: When a `Service` is registered, the `ApplicationContext` should inspect its constructor requirements (this may require manual metadata at first) and inject the required `Bean` dependencies from its container.

### ☐ Phase 4: The "Starter" Plugin System (Static Linking)

*This phase modularizes the framework.*

- [ ] **4.1. Define `IStarter` Interface**: Create the `IStarter` interface with a single method: `void initialize(ApplicationContext& context)`. 
- [ ] **4.2. Refactor Existing Modules**: Convert existing functionalities (e.g., Lua, Gateway, Logging) into distinct `Starter` implementations. Each starter will register its own `Services` and `Beans` with the context.
- [ ] **4.3. Update `main()`**: The entry point will now be very simple: create an `ApplicationContext`, create a list of `Starters`, and run them.

---

## 5. Future Vision & Long-Term Roadmap

- [ ] **Dynamic Plugin Loading**: Use `Boost.DLL` to allow the `ApplicationContext` to load `IStarter` implementations from `.so`/`.dll` files in a plugins directory at runtime.
- [ ] **Advanced Dependency Injection**: Explore integrating a library like `Boost.DI` or enhancing our own to support property-based injection or more advanced DI patterns.
- [ ] **Formal `Bean` Lifecycle**: Introduce finer-grained lifecycle hooks for all beans, such as `post_construct()`.

---

## 6. Key Design Decisions & Rationale

- **Why not use `Boost.DI` directly?**
    - To adhere to the principle of minimizing third-party dependencies and only relying on the official Boost distribution. We can build a sufficiently powerful DI mechanism for our needs using standard Boost libraries (`Any`, `DLL`, `TypeIndex`).

- **Why the `BaseConfig` -> `Clonable` -> `Reloadable` hierarchy?**
    - This provides maximum clarity and compile-time safety. It separates the *ability* to clone from the *intent* to be reloadable, making the design self-documenting and preventing developers from accidentally subscribing to a config that cannot be reloaded.

- **How to handle the Logger problem (and other global services)?**
    - Global services like logging should not force business-logic services to become `Reloadable`. The `Logger` system will subscribe to its own `LogConfig` changes independently. This decouples infrastructure concerns from business-logic concerns.

- **Dependency Direction**: The dependency flow is unidirectional: `Application` -> `ApplicationContext` -> `Services` / `ConfigManager`. The `ConfigManager` is a foundational service with no outgoing dependencies.

