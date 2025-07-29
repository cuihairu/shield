#include "shield/script/lua_vm_pool.hpp"
#include "shield/core/logger.hpp"
#include <sstream>
#include <filesystem>

namespace shield::script {

// PooledLuaVM implementation
PooledLuaVM::PooledLuaVM(const std::string& vm_id) 
    : vm_id_(vm_id)
    , last_used_(std::chrono::steady_clock::now()) {
    
    lua_engine_ = std::make_unique<LuaEngine>(vm_id + "_engine");
}

PooledLuaVM::~PooledLuaVM() {
    if (lua_engine_) {
        lua_engine_->stop();
    }
}

bool PooledLuaVM::initialize() {
    try {
        lua_engine_->init();
        lua_engine_->start();
        healthy_ = true;
        
        SHIELD_LOG_DEBUG << "PooledLuaVM " << vm_id_ << " initialized successfully";
        return true;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to initialize PooledLuaVM " << vm_id_ << ": " << e.what();
        healthy_ = false;
        return false;
    }
}

void PooledLuaVM::reset() {
    try {
        // Create a new Lua state to reset everything
        if (lua_engine_) {
            lua_engine_->stop();
            lua_engine_ = std::make_unique<LuaEngine>(vm_id_ + "_engine");
            lua_engine_->init();
            lua_engine_->start();
        }
        
        healthy_ = true;
        last_used_ = std::chrono::steady_clock::now();
        
        SHIELD_LOG_DEBUG << "PooledLuaVM " << vm_id_ << " reset successfully";
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to reset PooledLuaVM " << vm_id_ << ": " << e.what();
        healthy_ = false;
    }
}

// LuaVMPool implementation
LuaVMPool::LuaVMPool(const std::string& name, LuaVMPoolConfig config)
    : Component(name)
    , config_(std::move(config)) {
    
    // Validate configuration
    if (config_.initial_size < config_.min_size) {
        config_.initial_size = config_.min_size;
    }
    if (config_.max_size < config_.min_size) {
        config_.max_size = config_.min_size;
    }
    if (config_.initial_size > config_.max_size) {
        config_.initial_size = config_.max_size;
    }
    
    SHIELD_LOG_INFO << "LuaVMPool '" << name << "' created with config: "
                   << "initial=" << config_.initial_size 
                   << ", min=" << config_.min_size 
                   << ", max=" << config_.max_size;
}

LuaVMPool::~LuaVMPool() {
    if (state() == core::ComponentState::STARTED) {
        stop();
    }
}

void LuaVMPool::on_init() {
    SHIELD_LOG_INFO << "Initializing LuaVMPool: " << name();
}

void LuaVMPool::on_start() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Create initial VMs
    for (size_t i = 0; i < config_.initial_size; ++i) {
        auto vm = create_vm();
        if (vm && initialize_vm(vm.get())) {
            available_vms_.push(vm);
            all_vms_[vm->vm_id()] = vm;
        } else {
            SHIELD_LOG_ERROR << "Failed to create initial VM " << i;
        }
    }
    
    running_ = true;
    
    // Start cleanup thread
    cleanup_thread_ = std::thread(&LuaVMPool::cleanup_thread_func, this);
    
    SHIELD_LOG_INFO << "LuaVMPool started with " << available_vms_.size() << " VMs";
}

void LuaVMPool::on_stop() {
    running_ = false;
    
    // Wake up any waiting threads
    pool_condition_.notify_all();
    
    // Join cleanup thread
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
    // Clear all VMs
    std::lock_guard<std::mutex> lock(pool_mutex_);
    available_vms_ = {};
    all_vms_.clear();
    
    SHIELD_LOG_INFO << "LuaVMPool stopped";
}

std::shared_ptr<PooledLuaVM> LuaVMPool::create_vm() {
    std::string vm_id = name() + "_vm_" + std::to_string(vm_counter_++);
    return std::make_shared<PooledLuaVM>(vm_id);
}

bool LuaVMPool::initialize_vm(PooledLuaVM* vm) {
    if (!vm->initialize()) {
        return false;
    }
    
    // Preload scripts if configured
    if (config_.preload_scripts) {
        preload_scripts_to_vm(vm);
    }
    
    return true;
}

void LuaVMPool::preload_scripts_to_vm(PooledLuaVM* vm) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    
    // Load script files
    for (const auto& script_path : script_paths_) {
        if (!vm->engine()->load_script(script_path)) {
            SHIELD_LOG_WARN << "Failed to preload script " << script_path << " to VM " << vm->vm_id();
        }
    }
    
    // Load script content
    for (const auto& [name, content] : preloaded_scripts_) {
        if (!vm->engine()->execute_string(content)) {
            SHIELD_LOG_WARN << "Failed to preload script content '" << name << "' to VM " << vm->vm_id();
        }
    }
}

VMHandle LuaVMPool::acquire_vm(std::chrono::milliseconds timeout) {
    auto start_time = std::chrono::steady_clock::now();
    total_acquisitions_++;
    
    std::unique_lock<std::mutex> lock(pool_mutex_);
    
    // Wait for available VM or timeout
    bool success = pool_condition_.wait_for(lock, timeout, [this] {
        return !available_vms_.empty() || !running_;
    });
    
    if (!running_) {
        failed_acquisitions_++;
        return {}; // Pool is shutting down
    }
    
    if (!success || available_vms_.empty()) {
        // Try to expand pool if possible
        if (all_vms_.size() < config_.max_size) {
            auto vm = create_vm();
            if (vm && initialize_vm(vm.get())) {
                all_vms_[vm->vm_id()] = vm;
                available_vms_.push(vm);
                SHIELD_LOG_DEBUG << "Expanded pool to " << all_vms_.size() << " VMs";
            }
        }
        
        if (available_vms_.empty()) {
            failed_acquisitions_++;
            SHIELD_LOG_WARN << "Failed to acquire VM from pool - timeout or pool exhausted";
            return {};
        }
    }
    
    // Get VM from pool
    auto vm = available_vms_.front();
    available_vms_.pop();
    lock.unlock();
    
    // Mark as used and check health
    vm->mark_used();
    if (!vm->is_healthy()) {
        // Try to reset unhealthy VM
        vm->reset();
        if (!vm->is_healthy()) {
            SHIELD_LOG_ERROR << "VM " << vm->vm_id() << " is unhealthy and cannot be reset";
            failed_acquisitions_++;
            return {};
        }
    }
    
    // Update statistics
    auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    total_wait_time_ms_ = (total_wait_time_ms_ * (total_acquisitions_ - 1) + wait_time) / total_acquisitions_;
    
    // Create RAII handle
    return VMHandle(vm, [this, vm]() {
        return_vm(vm);
    });
}

void LuaVMPool::return_vm(std::shared_ptr<PooledLuaVM> vm) {
    if (!vm) return;
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Check if VM is still in our registry
    auto it = all_vms_.find(vm->vm_id());
    if (it == all_vms_.end()) {
        SHIELD_LOG_WARN << "Attempting to return unknown VM: " << vm->vm_id();
        return;
    }
    
    // Return healthy VMs to pool
    if (vm->is_healthy() && running_) {
        available_vms_.push(vm);
        pool_condition_.notify_one();
        SHIELD_LOG_DEBUG << "Returned VM " << vm->vm_id() << " to pool";
    } else {
        // Remove unhealthy VMs
        all_vms_.erase(it);
        SHIELD_LOG_INFO << "Removed unhealthy VM " << vm->vm_id() << " from pool";
    }
}

bool LuaVMPool::preload_script(const std::string& script_path) {
    if (!std::filesystem::exists(script_path)) {
        SHIELD_LOG_ERROR << "Script file does not exist: " << script_path;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    script_paths_.push_back(script_path);
    
    SHIELD_LOG_INFO << "Added script for preloading: " << script_path;
    return true;
}

bool LuaVMPool::preload_script_content(const std::string& script_name, const std::string& content) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    preloaded_scripts_[script_name] = content;
    
    SHIELD_LOG_INFO << "Added script content for preloading: " << script_name;
    return true;
}

void LuaVMPool::clear_preloaded_scripts() {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    script_paths_.clear();
    preloaded_scripts_.clear();
    
    SHIELD_LOG_INFO << "Cleared all preloaded scripts";
}

LuaVMPool::PoolStats LuaVMPool::get_stats() const {
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    
    PoolStats stats;
    stats.total_vms = all_vms_.size();
    stats.available_vms = available_vms_.size();
    stats.active_vms = stats.total_vms - stats.available_vms;
    stats.total_acquisitions = total_acquisitions_.load();
    stats.failed_acquisitions = failed_acquisitions_.load();
    stats.average_wait_time_ms = total_wait_time_ms_.load();
    
    return stats;
}

void LuaVMPool::resize_pool(size_t new_size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    if (new_size < config_.min_size) {
        new_size = config_.min_size;
    }
    if (new_size > config_.max_size) {
        new_size = config_.max_size;
    }
    
    size_t current_size = all_vms_.size();
    
    if (new_size > current_size) {
        // Expand pool
        size_t to_create = new_size - current_size;
        for (size_t i = 0; i < to_create; ++i) {
            auto vm = create_vm();
            if (vm && initialize_vm(vm.get())) {
                available_vms_.push(vm);
                all_vms_[vm->vm_id()] = vm;
            }
        }
        SHIELD_LOG_INFO << "Expanded pool from " << current_size << " to " << all_vms_.size() << " VMs";
    } else if (new_size < current_size) {
        // Shrink pool (remove only available VMs)
        size_t to_remove = std::min(current_size - new_size, available_vms_.size());
        for (size_t i = 0; i < to_remove; ++i) {
            if (!available_vms_.empty()) {
                auto vm = available_vms_.front();
                available_vms_.pop();
                all_vms_.erase(vm->vm_id());
            }
        }
        SHIELD_LOG_INFO << "Shrunk pool from " << current_size << " to " << all_vms_.size() << " VMs";
    }
}

void LuaVMPool::cleanup_idle_vms() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    if (all_vms_.size() <= config_.min_size) {
        return; // Don't shrink below minimum
    }
    
    auto now = std::chrono::steady_clock::now();
    std::queue<std::shared_ptr<PooledLuaVM>> new_available;
    size_t removed_count = 0;
    
    // Check available VMs for idle timeout
    while (!available_vms_.empty()) {
        auto vm = available_vms_.front();
        available_vms_.pop();
        
        auto idle_time = now - vm->last_used();
        if (idle_time > config_.idle_timeout && all_vms_.size() > config_.min_size) {
            // Remove idle VM
            all_vms_.erase(vm->vm_id());
            removed_count++;
        } else {
            // Keep VM
            new_available.push(vm);
        }
    }
    
    available_vms_ = std::move(new_available);
    
    if (removed_count > 0) {
        SHIELD_LOG_INFO << "Cleaned up " << removed_count << " idle VMs, pool size: " << all_vms_.size();
    }
}

void LuaVMPool::cleanup_thread_func() {
    SHIELD_LOG_DEBUG << "LuaVMPool cleanup thread started";
    
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30)); // Check every 30 seconds
        
        if (running_) {
            cleanup_idle_vms();
        }
    }
    
    SHIELD_LOG_DEBUG << "LuaVMPool cleanup thread stopped";
}

} // namespace shield::script