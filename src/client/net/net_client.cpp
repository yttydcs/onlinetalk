#include "client/net/net_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/log.h"

namespace onlinetalk::client {

namespace {

constexpr int kPollTimeoutMs = 100;

bool setNonBlocking(int fd, std::string* error) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    if (error) {
      *error = "fcntl(F_GETFL) failed";
    }
    return false;
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    if (error) {
      *error = "fcntl(F_SETFL) failed";
    }
    return false;
  }
  return true;
}

bool setSocketOptions(int fd, std::string* error) {
  int yes = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) {
    if (error) {
      *error = "setsockopt(TCP_NODELAY) failed";
    }
    return false;
  }
  if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0) {
    if (error) {
      *error = "setsockopt(SO_KEEPALIVE) failed";
    }
    return false;
  }
  return true;
}

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t readU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint64_t readU64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

bool peekHeader(const onlinetalk::common::ByteBuffer& buffer,
                onlinetalk::common::PacketHeader* header,
                std::string* error) {
  if (!header) {
    if (error) {
      *error = "header is null";
    }
    return false;
  }
  if (buffer.size() < onlinetalk::common::Codec::kHeaderSize) {
    return false;
  }
  const uint8_t* data = buffer.data();
  onlinetalk::common::PacketHeader parsed;
  parsed.magic = readU32(data);
  parsed.version = readU16(data + 4);
  parsed.type = readU16(data + 6);
  parsed.flags = readU32(data + 8);
  parsed.request_id = readU64(data + 12);
  parsed.meta_len = readU32(data + 20);
  parsed.bin_len = readU32(data + 24);

  if (parsed.magic != onlinetalk::common::PacketHeader::kMagic ||
      parsed.version != onlinetalk::common::PacketHeader::kVersion) {
    if (error) {
      *error = "invalid packet header";
    }
    return false;
  }
  if (parsed.meta_len > onlinetalk::common::Codec::kMaxMetaSize ||
      parsed.bin_len > onlinetalk::common::Codec::kMaxBinarySize) {
    if (error) {
      *error = "packet size too large";
    }
    return false;
  }

  *header = parsed;
  return true;
}

bool tryDecodePacket(onlinetalk::common::ByteBuffer& buffer,
                     onlinetalk::common::Packet* packet,
                     std::string* error) {
  if (!packet) {
    if (error) {
      *error = "packet is null";
    }
    return false;
  }
  onlinetalk::common::PacketHeader header;
  if (!peekHeader(buffer, &header, error)) {
    return false;
  }
  const size_t total = onlinetalk::common::Codec::kHeaderSize + header.meta_len + header.bin_len;
  if (buffer.size() < total) {
    return false;
  }
  if (!onlinetalk::common::Codec::decode(buffer, packet)) {
    if (error) {
      *error = "decode failed";
    }
    return false;
  }
  return true;
}

}  // namespace

NetClient::NetClient() = default;

NetClient::~NetClient() {
  stop();
}

bool NetClient::connectTo(const std::string& host, uint16_t port, std::string* error) {
  if (socket_fd_ >= 0) {
    if (error) {
      *error = "already connected";
    }
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  const std::string port_str = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
    if (error) {
      *error = "getaddrinfo failed for " + host + ":" + port_str;
    }
    return false;
  }

  int fd = -1;
  std::string last_error;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      last_error = "socket() failed";
      continue;
    }
    if (!setSocketOptions(fd, &last_error)) {
      ::close(fd);
      fd = -1;
      continue;
    }
    if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      if (!setNonBlocking(fd, &last_error)) {
        ::close(fd);
        fd = -1;
        continue;
      }
      break;
    }
    last_error = "connect() failed";
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(result);

  if (fd < 0) {
    if (error) {
      *error = last_error.empty() ? "failed to connect" : last_error;
    }
    return false;
  }

  socket_fd_ = fd;
  return true;
}

void NetClient::start() {
  if (running_ || socket_fd_ < 0) {
    return;
  }
  running_ = true;
  io_thread_ = std::thread([this]() { runLoop(); });
}

void NetClient::stop() {
  running_ = false;
  if (socket_fd_ >= 0) {
    ::shutdown(socket_fd_, SHUT_RDWR);
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  if (socket_fd_ >= 0) {
    ::close(socket_fd_);
    socket_fd_ = -1;
  }
}

bool NetClient::isRunning() const {
  return running_;
}

uint64_t NetClient::nextRequestId() {
  return next_request_id_++;
}

bool NetClient::sendPacket(onlinetalk::common::PacketType type,
                           uint64_t request_id,
                           const std::string& meta_json,
                           const std::vector<uint8_t>* binary) {
  if (socket_fd_ < 0) {
    return false;
  }
  onlinetalk::common::Packet packet;
  packet.header.type = static_cast<uint16_t>(type);
  packet.header.request_id = request_id;
  packet.meta_json = meta_json;
  if (binary) {
    packet.binary = *binary;
  }
  auto encoded = onlinetalk::common::Codec::encode(packet);
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    write_buffer_.insert(write_buffer_.end(), encoded.begin(), encoded.end());
  }
  return true;
}

bool NetClient::sendJson(onlinetalk::common::PacketType type,
                         uint64_t request_id,
                         const nlohmann::json& meta,
                         const std::vector<uint8_t>* binary) {
  return sendPacket(type, request_id, meta.dump(), binary);
}

bool NetClient::pollPacket(onlinetalk::common::Packet* out) {
  if (!out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(read_mutex_);
  if (incoming_.empty()) {
    return false;
  }
  *out = std::move(incoming_.front());
  incoming_.pop_front();
  return true;
}

std::string NetClient::lastError() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

void NetClient::runLoop() {
  while (running_) {
    short events = POLLIN;
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      if (write_offset_ < write_buffer_.size()) {
        events |= POLLOUT;
      }
    }

    pollfd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = events;
    const int rc = ::poll(&pfd, 1, kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      setLastError("poll failed");
      break;
    }
    if (rc == 0) {
      continue;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      setLastError("socket error");
      break;
    }
    if (pfd.revents & POLLIN) {
      std::string error;
      if (!readAvailable(&error)) {
        setLastError(error.empty() ? "read failed" : error);
        break;
      }
    }
    if (pfd.revents & POLLOUT) {
      std::string error;
      if (!flushWrite(&error)) {
        setLastError(error.empty() ? "write failed" : error);
        break;
      }
    }
  }
  running_ = false;
}

bool NetClient::flushWrite(std::string* error) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  while (write_offset_ < write_buffer_.size()) {
    const auto remaining = write_buffer_.size() - write_offset_;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto sent = ::send(socket_fd_,
                             write_buffer_.data() + write_offset_,
                             remaining,
                             flags);
    if (sent > 0) {
      write_offset_ += static_cast<size_t>(sent);
      continue;
    }
    if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (error) {
      *error = "send failed";
    }
    return false;
  }
  if (write_offset_ >= write_buffer_.size()) {
    write_buffer_.clear();
    write_offset_ = 0;
  }
  return true;
}

bool NetClient::readAvailable(std::string* error) {
  while (true) {
    uint8_t buffer[4096];
    const auto bytes = ::recv(socket_fd_, buffer, sizeof(buffer), 0);
    if (bytes > 0) {
      read_buffer_.append(buffer, static_cast<size_t>(bytes));
      continue;
    }
    if (bytes == 0) {
      if (error) {
        *error = "server closed";
      }
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    if (error) {
      *error = "recv failed";
    }
    return false;
  }

  while (true) {
    onlinetalk::common::Packet packet;
    std::string decode_error;
    if (!tryDecodePacket(read_buffer_, &packet, &decode_error)) {
      if (!decode_error.empty()) {
        if (error) {
          *error = decode_error;
        }
        return false;
      }
      break;
    }
    queuePacket(std::move(packet));
  }
  return true;
}

void NetClient::queuePacket(onlinetalk::common::Packet&& packet) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  incoming_.push_back(std::move(packet));
}

void NetClient::setLastError(const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
  }
  if (!error.empty()) {
    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                    "client network error: " + error);
  }
}

}  // namespace onlinetalk::client
