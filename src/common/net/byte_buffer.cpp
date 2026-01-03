#include "common/net/byte_buffer.h"

#include <algorithm>

namespace onlinetalk::common {

void ByteBuffer::append(const uint8_t* data, size_t size) {
  if (size == 0 || !data) {
    return;
  }
  buffer_.insert(buffer_.end(), data, data + size);
}

void ByteBuffer::append(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return;
  }
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void ByteBuffer::consume(size_t size) {
  if (size == 0) {
    return;
  }
  offset_ = std::min(offset_ + size, buffer_.size());
  if (offset_ > 0 && offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  }
  if (offset_ > 0 && offset_ >= buffer_.size() / 2) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset_));
    offset_ = 0;
  }
}

const uint8_t* ByteBuffer::data() const {
  return buffer_.data() + offset_;
}

size_t ByteBuffer::size() const {
  return buffer_.size() - offset_;
}

bool ByteBuffer::empty() const {
  return size() == 0;
}

}  // namespace onlinetalk::common
