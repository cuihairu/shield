#pragma once

#include <string>
namespace shield::core {

struct LogConfig {
    // Log level
    int level{0}; // corresponds to trace level
    
    // Log file path
    std::string log_file{"logs/shield.log"};
    
    // Maximum log file size (bytes)
    size_t max_file_size{1024 * 1024 * 100}; // 100MB
    
    // Number of log files to retain
    size_t max_files{5};
    
    // Whether to output to console simultaneously
    bool console_output{true};
    
    // Log format
    std::string pattern{"[%TimeStamp%] [%Severity%] %Message%"};
};

} // namespace shield::core 