#include "server/net/connection.h"

#include <cerrno>
#include <sys/socket.h>

namespace onlinetalk::server {

Connection::Connection(int fd) : fd_(fd) {}

int Connection::fd() const {
  return fd_;
}

onlinetalk::common::ByteBuffer& Connection::readBuffer() {
  return read_buffer_;
}

void Connection::queueWrite(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return;
  }
  write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
}

bool Connection::flushWrite() {
  while (write_offset_ < write_buffer_.size()) {
    const auto remaining = write_buffer_.size() - write_offset_;
    const auto sent = ::send(fd_,
                             write_buffer_.data() + write_offset_,
                             remaining,
                             0);
    if (sent > 0) {
      write_offset_ += static_cast<size_t>(sent);
      continue;
    }
    if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    return false;
  }
  if (write_offset_ >= write_buffer_.size()) {
    write_buffer_.clear();
    write_offset_ = 0;
  }
  return true;
}

bool Connection::hasPendingWrite() const {
  return write_offset_ < write_buffer_.size();
}

}  // namespace onlinetalk::server
