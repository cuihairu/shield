#pragma once

#include "shield/core/component.hpp"
#include "shield/script/lua_engine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace shield::script {

// VM pool configuration
struct LuaVMPoolConfig {
  size_t initial_size = 4;                         // Initial number of VMs
  size_t max_size = 32;                            // Maximum number of VMs
  size_t min_size = 2;                             // Minimum number of VMs
  std::chrono::milliseconds idle_timeout{30000};   // VM idle timeout
  std::chrono::milliseconds acquire_timeout{5000}; // Timeout for acquiring VM
  bool preload_scripts = true; // Whether to preload scripts on VM creation
};

// VM wrapper with lifecycle management
class PooledLuaVM {
public:
  explicit PooledLuaVM(const std::string &vm_id);
  ~PooledLuaVM();

  // Get the underlying Lua engine
  LuaEngine *engine() { return lua_engine_.get(); }

  // VM lifecycle
  bool initialize();
  void reset();
  bool is_healthy() const { return healthy_; }

  // Usage tracking
  void mark_used() {
    last_used_ = std::chrono::steady_clock::now();
    usage_count_++;
  }

  auto last_used() const { return last_used_; }
  size_t usage_count() const { return usage_count_; }
  const std::string &vm_id() const { return vm_id_; }

private:
  std::unique_ptr<LuaEngine> lua_engine_;
  std::string vm_id_;
  std::atomic<bool> healthy_{true};
  std::chrono::steady_clock::time_point last_used_;
  std::atomic<size_t> usage_count_{0};
};

// RAII wrapper for VM acquisition
class VMHandle {
public:
  VMHandle() = default;
  VMHandle(std::shared_ptr<PooledLuaVM> vm, std::function<void()> return_func)
      : vm_(std::move(vm)), return_func_(std::move(return_func)) {}

  // Move constructor and assignment
  VMHandle(VMHandle &&other) noexcept
      : vm_(std::move(other.vm_)), return_func_(std::move(other.return_func_)) {
    other.return_func_ = nullptr; // Prevent double return
  }

  VMHandle &operator=(VMHandle &&other) noexcept {
    if (this != &other) {
      release(); // Return current VM first
      vm_ = std::move(other.vm_);
      return_func_ = std::move(other.return_func_);
      other.return_func_ = nullptr;
    }
    return *this;
  }

  // Disable copy
  VMHandle(const VMHandle &) = delete;
  VMHandle &operator=(const VMHandle &) = delete;

  ~VMHandle() { release(); }

  // Access operators
  LuaEngine *operator->() const { return vm_ ? vm_->engine() : nullptr; }
  LuaEngine &operator*() const { return *vm_->engine(); }
  explicit operator bool() const { return vm_ != nullptr; }

  // Get the VM wrapper
  PooledLuaVM *vm() const { return vm_.get(); }

  // Manual release
  void release() {
    if (return_func_) {
      return_func_();
      return_func_ = nullptr;
    }
    vm_.reset();
  }

private:
  std::shared_ptr<PooledLuaVM> vm_;
  std::function<void()> return_func_;
};

// High-performance Lua VM pool for concurrent actor processing
class LuaVMPool : public core::Component {
public:
  explicit LuaVMPool(const std::string &name, LuaVMPoolConfig config = {});
  ~LuaVMPool();

  // VM lifecycle management
  VMHandle acquire_vm(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
  void return_vm(std::shared_ptr<PooledLuaVM> vm);

  // Script management
  bool preload_script(const std::string &script_path);
  bool preload_script_content(const std::string &script_name,
                              const std::string &content);
  void clear_preloaded_scripts();

  // Pool statistics
  struct PoolStats {
    size_t total_vms = 0;
    size_t available_vms = 0;
    size_t active_vms = 0;
    size_t total_acquisitions = 0;
    size_t failed_acquisitions = 0;
    double average_wait_time_ms = 0.0;
  };
  PoolStats get_stats() const;

  // Pool management
  void resize_pool(size_t new_size);
  void cleanup_idle_vms();

protected:
  void on_init() override;
  void on_start() override;
  void on_stop() override;

private:
  // VM creation and initialization
  std::shared_ptr<PooledLuaVM> create_vm();
  bool initialize_vm(PooledLuaVM *vm);
  void preload_scripts_to_vm(PooledLuaVM *vm);

  // Pool management
  void expand_pool();
  void shrink_pool();
  void cleanup_thread_func();

  // Thread-safe pool operations
  mutable std::mutex pool_mutex_;
  std::condition_variable pool_condition_;
  std::queue<std::shared_ptr<PooledLuaVM>> available_vms_;
  std::unordered_map<std::string, std::shared_ptr<PooledLuaVM>> all_vms_;

  // Configuration
  LuaVMPoolConfig config_;

  // Preloaded scripts
  std::mutex scripts_mutex_;
  std::unordered_map<std::string, std::string> preloaded_scripts_;
  std::vector<std::string> script_paths_;

  // Statistics
  mutable std::mutex stats_mutex_;
  std::atomic<size_t> total_acquisitions_{0};
  std::atomic<size_t> failed_acquisitions_{0};
  std::atomic<double> total_wait_time_ms_{0.0};

  // Lifecycle management
  std::atomic<bool> running_{false};
  std::thread cleanup_thread_;
  std::atomic<size_t> vm_counter_{0};
};

} // namespace shield::script