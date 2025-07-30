#pragma once

#include <string>

namespace shield::core {

// Forward declaration
class ApplicationContext;

// Base interface for all services, defining the lifecycle hooks.
class Service {
public:
    virtual ~Service() = default;

    // Called after the service is registered and dependencies are available.
    virtual void on_init(ApplicationContext& ctx) {}

    // Called after all services have been initialized.
    virtual void on_start() {}

    // Called when the application is shutting down.
    virtual void on_stop() {}

    // Returns the name of the service.
    virtual std::string name() const = 0;
};

// Base interface for services that can react to configuration changes.
class IReloadableService {
public:
    virtual ~IReloadableService() = default;

    // This method is called by the ApplicationContext after a successful config
    // reload.
    virtual void on_config_reloaded() = 0;
};

// Convenience class for creating a reloadable service.
class ReloadableService : public Service, public IReloadableService {
public:
    // Inherits from both Service and IReloadableService
};

}  // namespace shield::core