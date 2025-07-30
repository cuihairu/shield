#pragma once
#include <boost/log/trivial.hpp>
#include <memory>
#include <string>

#include "shield/log/log_config.hpp"

namespace shield::log {

class Logger {
public:
    static void init(const LogConfig &config);
    static void shutdown();
    static LogConfig::LogLevel level_from_string(const std::string &level_str);
    static void set_level(
        LogConfig::LogLevel level);  // Added for dynamic level change

private:
    static LogConfig config_;
};

}  // namespace shield::log

#define SHIELD_LOG_TRACE BOOST_LOG_TRIVIAL(trace)
#define SHIELD_LOG_DEBUG BOOST_LOG_TRIVIAL(debug)
#define SHIELD_LOG_INFO BOOST_LOG_TRIVIAL(info)
#define SHIELD_LOG_WARN BOOST_LOG_TRIVIAL(warning)
#define SHIELD_LOG_WARNING BOOST_LOG_TRIVIAL(warning)
#define SHIELD_LOG_ERROR BOOST_LOG_TRIVIAL(error)
#define SHIELD_LOG_FATAL BOOST_LOG_TRIVIAL(fatal)