#include "common/config.h"
#include "common/fs.h"
#include "common/log.h"
#include "server/net/tcp_server.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string resolveConfigPath(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--config") {
      return argv[i + 1];
    }
  }
  std::vector<std::string> candidates = {
      "config/server.json",
      "../config/server.json",
  };

  std::error_code error;
  const auto exe_path = std::filesystem::absolute(argv[0], error);
  if (!error) {
    const auto exe_dir = exe_path.parent_path();
    if (!exe_dir.empty()) {
      candidates.push_back((exe_dir / "config/server.json").lexically_normal().string());
      candidates.push_back((exe_dir / "../config/server.json").lexically_normal().string());
    }
  }

  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return candidates.front();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string config_path = resolveConfigPath(argc, argv);
    const auto config = onlinetalk::common::loadServerConfig(config_path);
    const auto level = onlinetalk::common::parseLogLevel(config.log_level);
    onlinetalk::common::Logger::setLevel(level);

    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                    "starting server with config: " + config_path);

    std::string error;
    if (!onlinetalk::common::ensureDirectory(config.data_dir, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "failed to create data_dir: " + error);
      return 1;
    }

    onlinetalk::server::TcpServer server(config);
    if (!server.start(&error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "server start failed: " + error);
      return 1;
    }

    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                    "server listening on " + config.bind_host + ":" +
                                        std::to_string(config.port));
    server.run();
    return 0;
  } catch (const onlinetalk::common::ConfigError& ex) {
    std::cerr << "config error: " << ex.what() << std::endl;
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
