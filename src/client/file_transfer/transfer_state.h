#pragma once

#include <cstdint>
#include <string>

namespace onlinetalk::client {

struct TransferState {
  std::string file_id;
  std::string file_name;
  int64_t total_size = 0;
  int64_t transferred = 0;
  bool done = false;
  bool failed = false;

  double progress() const {
    if (total_size <= 0) {
      return 0.0;
    }
    const double ratio = static_cast<double>(transferred) / static_cast<double>(total_size);
    if (ratio < 0.0) {
      return 0.0;
    }
    if (ratio > 1.0) {
      return 1.0;
    }
    return ratio;
  }
};

}  // namespace onlinetalk::client
