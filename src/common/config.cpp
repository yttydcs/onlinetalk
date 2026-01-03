#include "common/config.h"

#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace onlinetalk::common {

namespace {

std::string readFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw ConfigError("failed to open config file: " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

template <typename T>
T readRequired(const nlohmann::json& root, const std::string& key) {
  if (!root.contains(key)) {
    throw ConfigError("missing required config key: " + key);
  }
  try {
    return root.at(key).get<T>();
  } catch (const nlohmann::json::exception&) {
    throw ConfigError("invalid type for config key: " + key);
  }
}

template <typename T>
T readOptional(const nlohmann::json& root, const std::string& key, const T& fallback) {
  if (!root.contains(key)) {
    return fallback;
  }
  try {
    return root.at(key).get<T>();
  } catch (const nlohmann::json::exception&) {
    throw ConfigError("invalid type for config key: " + key);
  }
}

uint16_t readPort(const nlohmann::json& root, const std::string& key) {
  const int port = readRequired<int>(root, key);
  if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
    throw ConfigError("port out of range for key: " + key);
  }
  return static_cast<uint16_t>(port);
}

}  // namespace

ServerConfig loadServerConfig(const std::string& path) {
  const auto json = nlohmann::json::parse(readFile(path));
  ServerConfig cfg;
  cfg.bind_host = readRequired<std::string>(json, "bind_host");
  cfg.port = readPort(json, "port");
  cfg.data_dir = readRequired<std::string>(json, "data_dir");
  cfg.db_path = readRequired<std::string>(json, "db_path");
  cfg.log_level = readOptional<std::string>(json, "log_level", "info");
  cfg.thread_pool_size = readOptional<int>(json, "thread_pool_size", 4);
  cfg.max_clients = readOptional<int>(json, "max_clients", 1000);
  cfg.history_page_size = readOptional<int>(json, "history_page_size", 100);
  cfg.file_chunk_size = readOptional<int>(json, "file_chunk_size", 65536);

  if (cfg.thread_pool_size <= 0) {
    throw ConfigError("thread_pool_size must be positive");
  }
  if (cfg.max_clients <= 0) {
    throw ConfigError("max_clients must be positive");
  }
  if (cfg.history_page_size <= 0) {
    throw ConfigError("history_page_size must be positive");
  }
  if (cfg.file_chunk_size <= 0) {
    throw ConfigError("file_chunk_size must be positive");
  }
  return cfg;
}

ClientConfig loadClientConfig(const std::string& path) {
  const auto json = nlohmann::json::parse(readFile(path));
  ClientConfig cfg;
  cfg.server_host = readRequired<std::string>(json, "server_host");
  cfg.server_port = readPort(json, "server_port");
  cfg.data_dir = readRequired<std::string>(json, "data_dir");
  cfg.log_level = readOptional<std::string>(json, "log_level", "info");
  cfg.history_page_size = readOptional<int>(json, "history_page_size", 100);
  cfg.window_width = readOptional<int>(json, "window_width", 1024);
  cfg.window_height = readOptional<int>(json, "window_height", 720);
  cfg.emoji_font_path = readOptional<std::string>(json, "emoji_font_path", "");

  if (cfg.history_page_size <= 0) {
    throw ConfigError("history_page_size must be positive");
  }
  if (cfg.window_width <= 0 || cfg.window_height <= 0) {
    throw ConfigError("window dimensions must be positive");
  }
  return cfg;
}

}  // namespace onlinetalk::common
