#include "common/fs.h"

#include <filesystem>
#include <system_error>

namespace onlinetalk::common {

bool ensureDirectory(const std::string& path, std::string* error) {
  std::error_code code;
  if (path.empty()) {
    if (error) {
      *error = "path is empty";
    }
    return false;
  }
  const std::filesystem::path dir(path);
  if (std::filesystem::exists(dir, code)) {
    if (std::filesystem::is_directory(dir, code)) {
      return true;
    }
    if (error) {
      *error = "path exists but is not a directory";
    }
    return false;
  }
  if (std::filesystem::create_directories(dir, code)) {
    return true;
  }
  if (error) {
    *error = code.message();
  }
  return false;
}

}  // namespace onlinetalk::common
