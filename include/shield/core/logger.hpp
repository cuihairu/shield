#pragma once
#include "shield/core/log_config.hpp"
#include <boost/log/trivial.hpp>
#include <memory>
#include <string>

namespace shield::core {

class Logger {
public:
  static void init(const LogConfig &config);
  static void shutdown();
  static int level_from_string(const std::string &level_str);

private:
  static LogConfig config_;
};

} // namespace shield::core

#define SHIELD_LOG_TRACE BOOST_LOG_TRIVIAL(trace)
#define SHIELD_LOG_DEBUG BOOST_LOG_TRIVIAL(debug)
#define SHIELD_LOG_INFO BOOST_LOG_TRIVIAL(info)
#define SHIELD_LOG_WARN BOOST_LOG_TRIVIAL(warning)
#define SHIELD_LOG_WARNING BOOST_LOG_TRIVIAL(warning)
#define SHIELD_LOG_ERROR BOOST_LOG_TRIVIAL(error)
#define SHIELD_LOG_FATAL BOOST_LOG_TRIVIAL(fatal)