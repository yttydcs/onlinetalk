#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace onlinetalk::common {

class ByteBuffer {
 public:
  void append(const uint8_t* data, size_t size);
  void append(const std::vector<uint8_t>& data);
  void consume(size_t size);
  const uint8_t* data() const;
  size_t size() const;
  bool empty() const;

 private:
  std::vector<uint8_t> buffer_;
  size_t offset_ = 0;
};

}  // namespace onlinetalk::common
