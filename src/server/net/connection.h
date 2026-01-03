#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/net/byte_buffer.h"

namespace onlinetalk::server {

class Connection {
 public:
  explicit Connection(int fd);

  int fd() const;
  onlinetalk::common::ByteBuffer& readBuffer();
  void queueWrite(const std::vector<uint8_t>& data);
  bool flushWrite();
  bool hasPendingWrite() const;

 private:
  int fd_;
  onlinetalk::common::ByteBuffer read_buffer_;
  std::vector<uint8_t> write_buffer_;
  size_t write_offset_ = 0;
};

}  // namespace onlinetalk::server
