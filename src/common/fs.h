#pragma once

#include <string>

namespace onlinetalk::common {

bool ensureDirectory(const std::string& path, std::string* error);

}  // namespace onlinetalk::common
