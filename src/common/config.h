#pragma once

#include <cstdint>
#include <string>

namespace onlinetalk::common {

struct ServerConfig {
  std::string bind_host;
  uint16_t port = 0;
  std::string data_dir;
  std::string db_path;
  std::string log_level;
  int thread_pool_size = 4;
  int max_clients = 1000;
  int history_page_size = 100;
  int file_chunk_size = 65536;
};

struct ClientConfig {
  std::string server_host;
  uint16_t server_port = 0;
  std::string data_dir;
  std::string log_level;
  int history_page_size = 100;
  int window_width = 1024;
  int window_height = 720;
  std::string emoji_font_path;
};

class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

ServerConfig loadServerConfig(const std::string& path);
ClientConfig loadClientConfig(const std::string& path);

}  // namespace onlinetalk::common
