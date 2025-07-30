
# Shield 框架重构：TODO 与架构计划

本文档旨在阐述重构 Shield 项目的架构愿景和分阶段实施计划。我们的目标是将其从当前状态演进为一个现代、模块化、可扩展的 C++ 框架，其设计思想深受 Spring Boot 的深刻影响。

---

## 1. 顶层愿景与指导原则

我们的目标是构建一个“框架”，而不仅仅是一个“应用”。这意味着以下几点将是我们的优先考量：

- **模块化与可扩展性**: 新功能（如数据库访问、新网络协议）应能作为独立的插件（`Starter`）添加，而无需修改核心代码。
- **控制反转 (IoC)**: 由框架来管理组件的生命周期和依赖关系，而非组件自身。
- **依赖注入 (DI)**: 组件从框架接收其依赖，使其更易于测试和理解。
- **约定优于配置**: 框架应提供合理的默认设置，以减少样板代码。
- **明确的关注点分离**: 框架的每个部分都有单一、明确的职责。

---

## 2. 核心概念与术语定义

为确保清晰，我们将采用以下术语：

- **`ApplicationContext` (应用上下文)**: 框架的核心。一个管理所有 Beans 和 Services 生命周期的 IoC 容器。它负责实例化、组装（DI）和生命周期管理（`init`, `start`, `stop`）。此概念将取代旧的 `ComponentManager`。

- **`Service` (服务)**: 一个主要的、有状态的、长期存在的对象，提供一项核心业务能力。例如：`GatewayService`, `MetricsService`。服务拥有明确的生命周期。此概念将取代旧的 `Component`。

- **`ReloadableService` (可重载服务)**: 一种特殊的 `Service`，能够响应运行时的配置变更。

- **`Bean` (对象/组件)**: 任何由 `ApplicationContext` 管理的对象。这是一个比 `Service` 更细粒度的概念。一个 `Service` 是一个 `Bean`，但 `Bean` 也可以是一个简单的工具类、工厂或配置对象。

- **`Config` (配置对象)**: 一种专门用于承载配置数据的 `Bean`。它由 `ConfigManager` 填充。

- **`Starter` (启动器)**: 一个自包含的插件，负责向 `ApplicationContext` 注册一系列相关的 Beans 和 Services。例如，`LuaStarter` 会注册所有 Lua 脚本所需的服务和对象。

---

## 3. 详细架构蓝图

### 3.1. `ConfigManager`: 数据与通知中心

- **职责**: 只管理配置数据。
- **核心特性**:
    1.  **加载**: 从文件（`.yaml`, `.json` 等）读取配置，并加载到一个内部的 `boost::property_tree::ptree` 结构中。
    2.  **热加载**: 实现一个事务性的“克隆-验证-交换”机制，以安全地在运行时应用配置变更。
    3.  **通知**: 提供一个类型安全的发布/订阅系统（`subscribe_to_reloads<T>`），供系统的其他部分监听配置变更。
- **关键约束**: `ConfigManager` 对 `Services`、`ApplicationContext` 或任何其他业务逻辑组件 **一无所知**。它只处理数据和回调。

### 3.2. `Config` 的继承体系设计

为实现类型安全和清晰的意图，我们将设计如下继承链：

1.  `ComponentConfig` (建议重命名为 `BaseConfig`):
    - 所有配置 `Bean` 的抽象基类。
    - `virtual from_ptree(...) = 0`
    - `virtual supports_hot_reload() const { return false; }`

2.  `ClonableComponentConfig<Derived>`:
    - 继承自 `BaseConfig`。
    - 使用 CRTP 设计模式，提供一个具体的 `clone()` 实现。
    - **不** 重写 `supports_hot_reload()`。

3.  `ReloadableComponentConfig<Derived>`:
    - 继承自 `ClonableComponentConfig<Derived>`。
    - 其唯一目的是 `override supports_hot_reload() const { return true; }`。
    - 这提供了一种清晰的、自文档化的方式来标记一个 `Config` 是可热加载的。

### 3.3. `ApplicationContext`: 核心 IoC 容器

- **职责**: 管理对象的生命周期和依赖注入。
- **核心特性**:
    1.  **Bean/Service 注册表**: 包含一个中央仓库，用于存放所有被管理的对象，可能会使用 `std::unordered_map<std::string, boost::any>` 来存储命名的 `Bean`。
    2.  **生命周期管理**: 按照正确的顺序，调用所有已注册 `Service` 的生命周期钩子（`on_init`, `on_start`, `on_stop`）。
    3.  **“胶水”角色**: 它订阅 `ConfigManager` 的重载事件，并将其分派给正确的 `ReloadableService` 实例。

### 3.4. `Service` & `ReloadableService`

- **`Service` 接口**:
    - `virtual void on_init(ApplicationContext& ctx) {}`
    - `virtual void on_start() {}`
    - `virtual void on_stop() {}`
- **`ReloadableService` 接口**:
    - 继承自 `Service`。
    - `virtual void on_config_reloaded() = 0;`

---

## 4. 分阶段实施计划 (TODO 清单)

### ☐ 阶段一：配置系统重构 (奠定基础)

*此阶段专注于使配置系统变得健壮并为热加载做好准备，暂时不触及组件/服务模型。*

- [ ] **1.1. 清理 `ComponentConfig`**: 从基类和所有派生类中移除 `to_yaml` 方法。
- [ ] **1.2. 实现新的 `Config` 继承体系**:
    - [ ] 在 `shield/config/config.hpp` 中，定义 `ClonableComponentConfig<Derived>` 模板类。
    - [ ] 在同一文件中，定义 `ReloadableComponentConfig<Derived>` 模板类。
- [ ] **1.3. 重构现有的 Config 类**:
    - [ ] 将所有现存的配置对象（如 `GatewayConfig`, `LogConfig`）的基类更改为 `ReloadableComponentConfig<T>`。
    - [ ] 从这些类中移除所有手动的 `clone()` 或 `supports_hot_reload()` 实现。
- [ ] **1.4. 实现事务性热加载**: 在 `ConfigManager::reload_config` 中，实现“克隆-验证-交换”逻辑。
- [ ] **1.5. 实现回调机制**: 在 `ConfigManager` 中，实现 `subscribe_to_reloads<T>` 方法，并包含 `static_assert` 以确保它只接受 `ReloadableComponentConfig` 的派生类。

### ☐ 阶段二：引入 `ApplicationContext` 与 `Service` 生命周期

*此阶段将我们现有的组件更名为服务，并引入中央生命周期管理器。*

- [ ] **2.1. 核心概念重命名**:
    - [ ] `Component` -> `Service`
    - [ ] `ReloadableComponent` -> `ReloadableService`
    - [ ] `ComponentManager` -> `ApplicationContext`
- [ ] **2.2. 实现基础的 `ApplicationContext`**: 创建 `ApplicationContext` 类，并为其添加一个 `Service` 实例的注册表。它应按顺序管理服务，以保证正确的启动/关闭顺序。
- [ ] **2.3. 实现服务生命周期**: 在 `ApplicationContext` 中实现 `init_all()`, `start_all()`, `stop_all()` 方法，这些方法会调用已注册服务对应的 `on_init`, `on_start`, `on_stop` 钩子。
- [ ] **2.4. 集成 `ApplicationContext` 与 `ConfigManager`**:
    - [ ] `ApplicationContext` 将负责订阅 `ConfigManager`。
    - [ ] 当注册一个 `ReloadableService` 时，`ApplicationContext` 应自动创建一个回调，该回调在被触发时会调用对应服务的 `on_config_reloaded()` 方法。
- [ ] **2.5. 重构 `main()` / `server_command.cpp`**: 应用的入口点现在应主要与 `ApplicationContext` 交互，以注册服务并启动应用。

### ☐ 阶段三：实现基础的依赖注入 (DI)

*此阶段通过解耦服务之间的关系，使框架变得真正强大。*

- [ ] **3.1. 将 `ApplicationContext` 演进为 Bean 容器**: 增强内部注册表，使用 `boost::any` 来存储任意类型的、命名的 `Bean`，而不仅仅是 `Service`。
- [ ] **3.2. 实现构造函数注入 (手动解析)**: 当一个 `Service` 被注册时，`ApplicationContext` 应检查其构造函数需求（初期可能需要手动提供元数据），并从其容器中注入所需的 `Bean` 依赖。

### ☐ 阶段四：“Starter” 插件系统 (静态链接)

*此阶段将框架进行模块化。*

- [ ] **4.1. 定义 `IStarter` 接口**: 创建 `IStarter` 接口，其中包含一个方法：`void initialize(ApplicationContext& context)`。
- [ ] **4.2. 重构现有模块**: 将现有功能（如 Lua, Gateway, 日志）转换为独立的 `Starter` 实现。每个启动器将负责向上下文中注册自己的 `Services` 和 `Beans`。
- [ ] **4.3. 更新 `main()`**: 入口点现在将变得非常简单：创建一个 `ApplicationContext`，创建一个 `Starters` 列表，然后运行它们。

---

## 5. 未来愿景与长期路线图

- [ ] **动态插件加载**: 使用 `Boost.DLL` 允许 `ApplicationContext` 在运行时从一个 `plugins` 目录下的 `.so`/`.dll` 文件中加载 `IStarter` 实现。
- [ ] **高级依赖注入**: 探索集成像 `Boost.DI` 这样的库，或增强我们自己的实现以支持基于属性的注入等更高级的 DI 模式。
- [ ] **形式化的 `Bean` 生命周期**: 为所有 `Bean` 引入更细粒度的生命周期钩子，例如 `post_construct()`。

---

## 6. 关键设计决策与基本原理

- **为何不直接使用 `Boost.DI`?**
    - 为了遵循“最小化第三方依赖，尽量只依赖 Boost 官方发行版”的原则。我们可以使用标准的 Boost 库（`Any`, `DLL`, `TypeIndex`）构建一个满足我们需求的、足够强大的 DI 机制。

- **为何设计 `BaseConfig` -> `Clonable` -> `Reloadable` 的继承体系?**
    - 这提供了最大程度的清晰性和编译时安全。它将“克隆的**能力**”与“热加载的**意图**”分离开来，使设计自文档化，并防止开发者意外地订阅一个无法被重载的配置。

- **如何处理 Logger 问题（以及其他全局服务）?**
    - 像日志这样的全局服务，不应强迫业务逻辑服务也变为 `Reloadable`。`Logger` 系统将独立订阅其自身的 `LogConfig` 变更。这将基础设施的关注点与业务逻辑的关注点解耦。

- **依赖方向**: 依赖流是单向的：`Application` -> `ApplicationContext` -> `Services` / `ConfigManager`。`ConfigManager` 是一个基础服务，没有任何对外的依赖。

