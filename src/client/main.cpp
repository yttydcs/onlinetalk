#include "client/file_transfer/file_transfer_manager.h"
#include "client/net/client_api.h"
#include "client/net/net_client.h"
#include "client/state/client_state.h"
#include "client/ui/ui_app.h"
#include "common/config.h"
#include "common/fs.h"
#include "common/log.h"

#include <SDL.h>
#include <SDL_ttf.h>

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
      "config/client.json",
      "../config/client.json",
  };

  std::error_code error;
  const auto exe_path = std::filesystem::absolute(argv[0], error);
  if (!error) {
    const auto exe_dir = exe_path.parent_path();
    if (!exe_dir.empty()) {
      candidates.push_back((exe_dir / "config/client.json").lexically_normal().string());
      candidates.push_back((exe_dir / "../config/client.json").lexically_normal().string());
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
    const auto config = onlinetalk::common::loadClientConfig(config_path);
    const auto level = onlinetalk::common::parseLogLevel(config.log_level);
    onlinetalk::common::Logger::setLevel(level);

    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                    "starting client with config: " + config_path);

    std::string error;
    if (!onlinetalk::common::ensureDirectory(config.data_dir, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "failed to create data_dir: " + error);
      return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      std::string("SDL init failed: ") + SDL_GetError());
      return 1;
    }
    if (TTF_Init() != 0) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      std::string("SDL_ttf init failed: ") + TTF_GetError());
      SDL_Quit();
      return 1;
    }

    onlinetalk::client::NetClient net;
    if (net.connectTo(config.server_host, config.server_port, &error)) {
      net.start();
    } else {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                      "initial connect failed: " + error);
    }

    onlinetalk::client::ClientApi api(net);
    onlinetalk::client::ClientState state;
    onlinetalk::client::FileTransferManager transfers(config.data_dir);

    onlinetalk::client::UiApp app(config, net, api, state, transfers);
    if (!app.init(&error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "UI init failed: " + error);
      net.stop();
      TTF_Quit();
      SDL_Quit();
      return 1;
    }

    app.run();
    app.shutdown();
    net.stop();
    TTF_Quit();
    SDL_Quit();
    return 0;
  } catch (const onlinetalk::common::ConfigError& ex) {
    std::cerr << "config error: " << ex.what() << std::endl;
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
