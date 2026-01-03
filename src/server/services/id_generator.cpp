#include "server/services/id_generator.h"

#include <array>
#include <random>
#include <sstream>
#include <iomanip>

namespace onlinetalk::server {

std::string generateId() {
  std::array<uint8_t, 16> bytes{};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : bytes) {
    b = static_cast<uint8_t>(dist(gen));
  }

  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (const auto b : bytes) {
    ss << std::setw(2) << static_cast<int>(b);
  }
  return ss.str();
}

}  // namespace onlinetalk::server
