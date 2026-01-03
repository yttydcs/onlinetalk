#include "server/net/tcp_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "common/protocol/codec.h"

namespace onlinetalk::server {

namespace {

constexpr int kMaxEvents = 64;
constexpr size_t kMaxFieldLength = 64;

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
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
    if (error) {
      *error = "setsockopt(SO_REUSEADDR) failed";
    }
    return false;
  }
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
  return true;
}

bool setClientSocketOptions(int fd, std::string* error) {
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

int createListenSocket(const std::string& host, uint16_t port, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  const std::string port_str = std::to_string(port);
  if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(), &hints, &result) != 0) {
    if (error) {
      *error = "getaddrinfo failed for " + host + ":" + port_str;
    }
    return -1;
  }

  int listen_fd = -1;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    listen_fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (listen_fd < 0) {
      continue;
    }
    if (!setSocketOptions(listen_fd, error)) {
      ::close(listen_fd);
      listen_fd = -1;
      continue;
    }
    if (!setNonBlocking(listen_fd, error)) {
      ::close(listen_fd);
      listen_fd = -1;
      continue;
    }
    if (::bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      if (::listen(listen_fd, SOMAXCONN) == 0) {
        break;
      }
    }
    ::close(listen_fd);
    listen_fd = -1;
  }
  ::freeaddrinfo(result);
  if (listen_fd < 0 && error && error->empty()) {
    *error = "failed to bind/listen on " + host + ":" + port_str;
  }
  return listen_fd;
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

bool parseJson(const std::string& text, nlohmann::json* out, std::string* error) {
  try {
    *out = nlohmann::json::parse(text);
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("invalid json: ") + ex.what();
    }
    return false;
  }
}

bool validateField(const std::string& value, const std::string& field, std::string* error) {
  if (value.empty()) {
    if (error) {
      *error = field + " is required";
    }
    return false;
  }
  if (value.size() > kMaxFieldLength) {
    if (error) {
      *error = field + " too long";
    }
    return false;
  }
  return true;
}

}  // namespace

TcpServer::TcpServer(const onlinetalk::common::ServerConfig& config) : config_(config) {}

TcpServer::~TcpServer() {
  stop();
}

bool TcpServer::start(std::string* error) {
  if (!setupListener(error)) {
    return false;
  }
  epoll_fd_ = ::epoll_create1(0);
  if (epoll_fd_ < 0) {
    if (error) {
      *error = "epoll_create1 failed";
    }
    return false;
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = listen_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) != 0) {
    if (error) {
      *error = "epoll_ctl add listen fd failed";
    }
    return false;
  }

  running_ = true;
  return true;
}

void TcpServer::run() {
  std::vector<epoll_event> events(kMaxEvents);
  while (running_) {
    const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 1000);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "epoll_wait failed");
      break;
    }
    for (int i = 0; i < count; ++i) {
      const int fd = events[i].data.fd;
      const uint32_t ev = events[i].events;
      if (fd == listen_fd_) {
        acceptConnections();
        continue;
      }

      auto it = connections_.find(fd);
      if (it == connections_.end()) {
        continue;
      }
      if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        disconnect(fd);
        continue;
      }
      if (ev & EPOLLIN) {
        if (!handleRead(*it->second)) {
          disconnect(fd);
          continue;
        }
      }
      if (ev & EPOLLOUT) {
        if (!handleWrite(*it->second)) {
          disconnect(fd);
          continue;
        }
      }
      updateEpollEvents(fd, it->second->hasPendingWrite());
    }
  }
}

void TcpServer::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  for (auto& entry : connections_) {
    ::close(entry.first);
  }
  connections_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

bool TcpServer::setupListener(std::string* error) {
  listen_fd_ = createListenSocket(config_.bind_host, config_.port, error);
  if (listen_fd_ < 0) {
    return false;
  }
  return true;
}

void TcpServer::acceptConnections() {
  while (true) {
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    const int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Error,
                                      "accept failed");
      break;
    }

    if (static_cast<int>(connections_.size()) >= config_.max_clients) {
      ::close(client_fd);
      continue;
    }

    std::string error;
    if (!setNonBlocking(client_fd, &error) || !setClientSocketOptions(client_fd, &error)) {
      ::close(client_fd);
      continue;
    }

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = client_fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) != 0) {
      ::close(client_fd);
      continue;
    }

    connections_.emplace(client_fd, std::make_unique<Connection>(client_fd));
    sessions_.addConnection(client_fd);
    onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                    "client connected fd=" + std::to_string(client_fd));
  }
}

bool TcpServer::handleRead(Connection& conn) {
  while (true) {
    uint8_t buffer[4096];
    const auto bytes = ::recv(conn.fd(), buffer, sizeof(buffer), 0);
    if (bytes > 0) {
      conn.readBuffer().append(buffer, static_cast<size_t>(bytes));
      continue;
    }
    if (bytes == 0) {
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return false;
  }
  return processPackets(conn);
}

bool TcpServer::handleWrite(Connection& conn) {
  return conn.flushWrite();
}

bool TcpServer::processPackets(Connection& conn) {
  while (true) {
    onlinetalk::common::Packet packet;
    std::string error;
    if (!tryDecodePacket(conn, &packet, &error)) {
      if (!error.empty()) {
        onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                        "protocol error: " + error);
        return false;
      }
      break;
    }
    const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
    switch (type) {
      case onlinetalk::common::PacketType::AuthLogin:
      case onlinetalk::common::PacketType::AuthRegister:
        handleAuth(conn, packet);
        break;
      default:
        onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                        "unhandled packet type: " + std::to_string(packet.header.type));
        break;
    }
  }
  return true;
}

bool TcpServer::tryDecodePacket(Connection& conn, onlinetalk::common::Packet* packet, std::string* error) {
  if (!packet) {
    if (error) {
      *error = "packet is null";
    }
    return false;
  }
  onlinetalk::common::PacketHeader header;
  if (!peekHeader(conn.readBuffer(), &header, error)) {
    return false;
  }
  const size_t total = onlinetalk::common::Codec::kHeaderSize + header.meta_len + header.bin_len;
  if (conn.readBuffer().size() < total) {
    return false;
  }
  if (!onlinetalk::common::Codec::decode(conn.readBuffer(), packet)) {
    if (error) {
      *error = "decode failed";
    }
    return false;
  }
  return true;
}

bool TcpServer::peekHeader(const onlinetalk::common::ByteBuffer& buffer,
                           onlinetalk::common::PacketHeader* header,
                           std::string* error) const {
  if (buffer.size() < onlinetalk::common::Codec::kHeaderSize) {
    return false;
  }
  const uint8_t* data = buffer.data();
  header->magic = readU32(data);
  header->version = readU16(data + 4);
  header->type = readU16(data + 6);
  header->flags = readU32(data + 8);
  header->request_id = readU64(data + 12);
  header->meta_len = readU32(data + 20);
  header->bin_len = readU32(data + 24);

  if (header->magic != onlinetalk::common::PacketHeader::kMagic) {
    if (error) {
      *error = "invalid magic";
    }
    return false;
  }
  if (header->version != onlinetalk::common::PacketHeader::kVersion) {
    if (error) {
      *error = "unsupported version";
    }
    return false;
  }
  if (header->meta_len > onlinetalk::common::Codec::kMaxMetaSize ||
      header->bin_len > onlinetalk::common::Codec::kMaxBinarySize) {
    if (error) {
      *error = "payload too large";
    }
    return false;
  }
  return true;
}

void TcpServer::handleAuth(Connection& conn, const onlinetalk::common::Packet& packet) {
  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_JSON", error);
    return;
  }

  const auto user_id = meta.value("user_id", "");
  const auto nickname = meta.value("nickname", "");
  const auto password = meta.value("password", "");

  if (!validateField(user_id, "user_id", &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_USER_ID", error);
    return;
  }
  if (!validateField(nickname, "nickname", &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_NICKNAME", error);
    return;
  }
  if (!validateField(password, "password", &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_PASSWORD", error);
    return;
  }

  if (!sessions_.login(conn.fd(), user_id, nickname, &error)) {
    sendAuthError(conn, packet.header.request_id, "LOGIN_FAILED", error);
    return;
  }

  onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                  "login ok: " + user_id);
  sendAuthOk(conn, packet.header.request_id);
  broadcastUserList();
}

void TcpServer::sendAuthError(Connection& conn,
                              uint64_t request_id,
                              const std::string& code,
                              const std::string& message) {
  nlohmann::json meta;
  meta["code"] = code;
  meta["message"] = message;
  auto packet = buildPacket(onlinetalk::common::PacketType::AuthError, request_id, meta.dump(), nullptr);
  conn.queueWrite(packet);
}

void TcpServer::sendAuthOk(Connection& conn, uint64_t request_id) {
  nlohmann::json meta;
  const auto* session = sessions_.getSession(conn.fd());
  if (session) {
    meta["user_id"] = session->user_id;
    meta["nickname"] = session->nickname;
  }
  auto users = sessions_.onlineUsers();
  nlohmann::json user_list = nlohmann::json::array();
  for (const auto& user : users) {
    nlohmann::json item;
    item["user_id"] = user.user_id;
    item["nickname"] = user.nickname;
    user_list.push_back(std::move(item));
  }
  meta["online_users"] = std::move(user_list);
  auto packet = buildPacket(onlinetalk::common::PacketType::AuthOk, request_id, meta.dump(), nullptr);
  conn.queueWrite(packet);
}

void TcpServer::broadcastUserList() {
  nlohmann::json meta;
  auto users = sessions_.onlineUsers();
  nlohmann::json user_list = nlohmann::json::array();
  for (const auto& user : users) {
    nlohmann::json item;
    item["user_id"] = user.user_id;
    item["nickname"] = user.nickname;
    user_list.push_back(std::move(item));
  }
  meta["users"] = std::move(user_list);

  const auto payload = meta.dump();
  for (auto& entry : connections_) {
    if (!sessions_.isLoggedIn(entry.first)) {
      continue;
    }
    entry.second->queueWrite(buildPacket(onlinetalk::common::PacketType::UserListUpdate, 0, payload, nullptr));
    updateEpollEvents(entry.first, true);
  }
}

std::vector<uint8_t> TcpServer::buildPacket(onlinetalk::common::PacketType type,
                                            uint64_t request_id,
                                            const std::string& meta_json,
                                            const std::vector<uint8_t>* binary) {
  onlinetalk::common::Packet packet;
  packet.header.type = static_cast<uint16_t>(type);
  packet.header.request_id = request_id;
  packet.meta_json = meta_json;
  if (binary) {
    packet.binary = *binary;
  }
  return onlinetalk::common::Codec::encode(packet);
}

void TcpServer::updateEpollEvents(int fd, bool want_write) {
  epoll_event event{};
  event.events = EPOLLIN | EPOLLRDHUP;
  if (want_write) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fd;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
}

void TcpServer::disconnect(int fd) {
  sessions_.removeConnection(fd);
  connections_.erase(fd);
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                  "client disconnected fd=" + std::to_string(fd));
  broadcastUserList();
}

}  // namespace onlinetalk::server
