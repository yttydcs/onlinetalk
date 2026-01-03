#pragma once

#include <mutex>
#include <string>

namespace onlinetalk::common {

enum class LogLevel {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3
};

class Logger {
 public:
  static void setLevel(LogLevel level);
  static LogLevel level();
  static void log(LogLevel level, const std::string& message);

 private:
  static std::mutex mutex_;
  static LogLevel level_;
};

LogLevel parseLogLevel(const std::string& value);
const char* toString(LogLevel level);

}  // namespace onlinetalk::common
