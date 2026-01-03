#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onlinetalk::common {

std::string sha256Hex(const std::vector<uint8_t>& data);
std::string sha256HexFile(const std::string& path, std::string* error);

}  // namespace onlinetalk::common
