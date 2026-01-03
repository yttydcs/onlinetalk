#include "common/config.h"
#include "common/fs.h"
#include "common/log.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <iostream>
#include <string>

namespace {

std::string resolveConfigPath(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--config") {
      return argv[i + 1];
    }
  }
  return "config/client.json";
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

    SDL_Window* window = SDL_CreateWindow("OnlineTalk",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          config.window_width,
                                          config.window_height,
                                          SDL_WINDOW_SHOWN);
    if (!window) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      std::string("SDL_CreateWindow failed: ") + SDL_GetError());
      TTF_Quit();
      SDL_Quit();
      return 1;
    }

    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                    "client UI not implemented yet");

    SDL_Delay(500);
    SDL_DestroyWindow(window);
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
