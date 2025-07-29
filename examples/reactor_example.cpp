#include "shield/core/reactor.hpp"
#include "shield/core/logger.hpp"
#include <iostream>
#include <chrono>
#include <thread>

using namespace shield::core;
using namespace std::chrono_literals;

void heavy_computation(int id) {
    SHIELD_LOG_INFO << "Task " << id << " started in thread " << std::this_thread::get_id();
    
    // 模拟耗时计算
    std::this_thread::sleep_for(2s);
    
    SHIELD_LOG_INFO << "Task " << id << " completed in thread " << std::this_thread::get_id();
}

int main() {
    // 配置日志系统
    LogConfig log_config;
    log_config.level = 1;  // 对应 debug 级别
    log_config.log_file = "logs/reactor_example.log";  // 设置日志文件路径
    log_config.console_output = true;  // 同时输出到控制台
    
    // 初始化日志系统
    Logger::init(log_config);
    
    // 创建一个有4个工作线程的 Reactor
    Reactor reactor(4);
    
    SHIELD_LOG_INFO << "Main thread: " << std::this_thread::get_id();
    
    // 提交10个任务
    for (int i = 0; i < 10; ++i) {
        SHIELD_LOG_DEBUG << "Submitting task " << i;
        reactor.submit_task([i]() { heavy_computation(i); });
    }
    
    // 在主线程中运行事件循环
    reactor.run();
    
    // 关闭日志系统
    Logger::shutdown();
    
    return 0;
} 