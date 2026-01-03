#include "common/log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace onlinetalk::common {

std::mutex Logger::mutex_;
LogLevel Logger::level_ = LogLevel::Info;

void Logger::setLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  level_ = level;
}

LogLevel Logger::level() {
  std::lock_guard<std::mutex> lock(mutex_);
  return level_;
}

void Logger::log(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<int>(level) < static_cast<int>(level_)) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &now_time);
#else
  localtime_r(&now_time, &tm_snapshot);
#endif

  std::ostringstream ss;
  ss << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
  std::cout << ss.str() << " [" << toString(level) << "] " << message << std::endl;
}

LogLevel parseLogLevel(const std::string& value) {
  std::string lowered = value;
  for (auto& ch : lowered) {
    ch = static_cast<char>(::tolower(ch));
  }
  if (lowered == "debug") {
    return LogLevel::Debug;
  }
  if (lowered == "warn" || lowered == "warning") {
    return LogLevel::Warn;
  }
  if (lowered == "error") {
    return LogLevel::Error;
  }
  return LogLevel::Info;
}

const char* toString(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
    default:
      return "INFO";
  }
}

}  // namespace onlinetalk::common
