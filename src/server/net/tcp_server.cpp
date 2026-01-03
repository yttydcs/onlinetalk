#include "server/net/tcp_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <algorithm>
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
constexpr size_t kMaxContentLength = 4096;
constexpr size_t kMaxFileNameLength = 255;
constexpr size_t kSha256HexLength = 64;

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

bool validateField(const std::string& value, const std::string& field, size_t max_len, std::string* error) {
  if (value.empty()) {
    if (error) {
      *error = field + " is required";
    }
    return false;
  }
  if (value.size() > max_len) {
    if (error) {
      *error = field + " too long";
    }
    return false;
  }
  return true;
}

}  // namespace

TcpServer::TcpServer(const onlinetalk::common::ServerConfig& config)
    : config_(config),
      database_(),
      auth_service_(database_),
      group_service_(database_),
      message_service_(database_),
      file_service_(database_, config_.data_dir, config_.file_chunk_size) {}

TcpServer::~TcpServer() {
  stop();
}

bool TcpServer::start(std::string* error) {
  if (!initDatabase(error)) {
    return false;
  }
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
      case onlinetalk::common::PacketType::GroupCreate:
      case onlinetalk::common::PacketType::GroupJoin:
      case onlinetalk::common::PacketType::GroupLeave:
      case onlinetalk::common::PacketType::GroupAdmin:
        handleGroup(conn, packet);
        break;
      case onlinetalk::common::PacketType::MessageSend:
        handleMessage(conn, packet);
        break;
      case onlinetalk::common::PacketType::FileOffer:
      case onlinetalk::common::PacketType::FileUploadChunk:
      case onlinetalk::common::PacketType::FileUploadDone:
      case onlinetalk::common::PacketType::FileDownloadRequest:
        handleFile(conn, packet);
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
  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  if (type == onlinetalk::common::PacketType::AuthRegister) {
    handleRegister(conn, packet);
    return;
  }
  handleLogin(conn, packet);
}

void TcpServer::handleRegister(Connection& conn, const onlinetalk::common::Packet& packet) {
  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_JSON", error);
    return;
  }

  const auto user_id = meta.value("user_id", "");
  const auto nickname = meta.value("nickname", "");
  const auto password = meta.value("password", "");

  if (!validateField(user_id, "user_id", kMaxFieldLength, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_USER_ID", error);
    return;
  }
  if (!validateField(nickname, "nickname", kMaxFieldLength, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_NICKNAME", error);
    return;
  }
  if (!validateField(password, "password", kMaxFieldLength, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_PASSWORD", error);
    return;
  }

  if (!auth_service_.registerUser(user_id, nickname, password, &error)) {
    sendAuthError(conn, packet.header.request_id, "REGISTER_FAILED", error);
    return;
  }

  nlohmann::json extra;
  extra["registered"] = true;
  extra["logged_in"] = false;
  sendResponse(conn,
               onlinetalk::common::PacketType::AuthOk,
               packet.header.request_id,
               "ok",
               "",
               "",
               extra.dump());
}

void TcpServer::handleLogin(Connection& conn, const onlinetalk::common::Packet& packet) {
  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_JSON", error);
    return;
  }

  const auto user_id = meta.value("user_id", "");
  const auto password = meta.value("password", "");

  if (!validateField(user_id, "user_id", kMaxFieldLength, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_USER_ID", error);
    return;
  }
  if (!validateField(password, "password", kMaxFieldLength, &error)) {
    sendAuthError(conn, packet.header.request_id, "INVALID_PASSWORD", error);
    return;
  }

  AuthUser user;
  if (!auth_service_.loginUser(user_id, password, &user, &error)) {
    sendAuthError(conn, packet.header.request_id, "LOGIN_FAILED", error);
    return;
  }
  if (!sessions_.login(conn.fd(), user.user_id, user.nickname, &error)) {
    sendAuthError(conn, packet.header.request_id, "LOGIN_FAILED", error);
    return;
  }

  onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Info,
                                  "login ok: " + user.user_id);
  sendAuthOk(conn, packet.header.request_id);
  broadcastUserList();
  deliverOfflineMessages(user.user_id, conn);
  deliverOfflineFiles(user.user_id, conn);
}

void TcpServer::handleGroup(Connection& conn, const onlinetalk::common::Packet& packet) {
  const auto* session = sessions_.getSession(conn.fd());
  if (!session || !session->logged_in) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "NOT_LOGGED_IN",
                 "login required",
                 "");
    return;
  }

  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "INVALID_JSON",
                 error,
                 "");
    return;
  }

  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  if (type == onlinetalk::common::PacketType::GroupCreate) {
    const auto name = meta.value("name", "");
    if (!validateField(name, "name", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_NAME", error, "");
      return;
    }
    std::string group_id;
    if (!group_service_.createGroup(session->user_id, name, &group_id, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "CREATE_FAILED", error, "");
      return;
    }
    nlohmann::json extra;
    extra["group_id"] = group_id;
    extra["name"] = name;
    sendResponse(conn, type, packet.header.request_id, "ok", "", "", extra.dump());
    return;
  }

  if (type == onlinetalk::common::PacketType::GroupJoin) {
    const auto group_id = meta.value("group_id", "");
    if (!validateField(group_id, "group_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_GROUP_ID", error, "");
      return;
    }
    if (!group_service_.joinGroup(session->user_id, group_id, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "JOIN_FAILED", error, "");
      return;
    }
    sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
    return;
  }

  if (type == onlinetalk::common::PacketType::GroupLeave) {
    const auto group_id = meta.value("group_id", "");
    if (!validateField(group_id, "group_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_GROUP_ID", error, "");
      return;
    }
    if (!group_service_.leaveGroup(session->user_id, group_id, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "LEAVE_FAILED", error, "");
      return;
    }
    sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
    return;
  }

  if (type == onlinetalk::common::PacketType::GroupAdmin) {
    const auto action = meta.value("action", "");
    const auto group_id = meta.value("group_id", "");
    if (!validateField(action, "action", kMaxFieldLength, &error) ||
        !validateField(group_id, "group_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_REQUEST", error, "");
      return;
    }

    if (action == "rename") {
      const auto new_name = meta.value("name", "");
      if (!validateField(new_name, "name", kMaxFieldLength, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "INVALID_NAME", error, "");
        return;
      }
      if (!group_service_.renameGroup(session->user_id, group_id, new_name, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "RENAME_FAILED", error, "");
        return;
      }
      sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
      return;
    }

    if (action == "kick") {
      const auto target = meta.value("target_user_id", "");
      if (!validateField(target, "target_user_id", kMaxFieldLength, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "INVALID_TARGET", error, "");
        return;
      }
      if (!group_service_.kickUser(session->user_id, group_id, target, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "KICK_FAILED", error, "");
        return;
      }
      sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
      return;
    }

    if (action == "dissolve") {
      if (!group_service_.dissolveGroup(session->user_id, group_id, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "DISSOLVE_FAILED", error, "");
        return;
      }
      sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
      return;
    }

    if (action == "promote" || action == "demote") {
      const auto target = meta.value("target_user_id", "");
      if (!validateField(target, "target_user_id", kMaxFieldLength, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "INVALID_TARGET", error, "");
        return;
      }
      const bool make_admin = (action == "promote");
      if (!group_service_.setAdmin(session->user_id, group_id, target, make_admin, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "ADMIN_FAILED", error, "");
        return;
      }
      sendResponse(conn, type, packet.header.request_id, "ok", "", "", "");
      return;
    }

    sendResponse(conn, type, packet.header.request_id, "error", "UNKNOWN_ACTION", "unsupported action", "");
    return;
  }
}

void TcpServer::handleMessage(Connection& conn, const onlinetalk::common::Packet& packet) {
  const auto* session = sessions_.getSession(conn.fd());
  if (!session || !session->logged_in) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "NOT_LOGGED_IN",
                 "login required",
                 "");
    return;
  }

  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "INVALID_JSON",
                 error,
                 "");
    return;
  }

  const auto conversation_type = meta.value("conversation_type", "");
  const auto conversation_id = meta.value("conversation_id", "");
  const auto content = meta.value("content", "");

  if (!validateField(conversation_type, "conversation_type", kMaxFieldLength, &error) ||
      !validateField(conversation_id, "conversation_id", kMaxFieldLength, &error) ||
      !validateField(content, "content", kMaxContentLength, &error)) {
    sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                 "error", "INVALID_REQUEST", error, "");
    return;
  }

  std::vector<std::string> recipients;
  if (conversation_type == "private") {
    std::string exists_error;
    const bool exists = auth_service_.userExists(conversation_id, &exists_error);
    if (!exists_error.empty()) {
      sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                   "error", "USER_LOOKUP_FAILED", exists_error, "");
      return;
    }
    if (!exists) {
      sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                   "error", "TARGET_NOT_FOUND", "target user not found", "");
      return;
    }
    recipients.push_back(conversation_id);
  } else if (conversation_type == "group") {
    std::string role;
    if (!group_service_.getUserRole(session->user_id, conversation_id, &role, &error)) {
      sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                   "error", "NOT_IN_GROUP", error, "");
      return;
    }
    if (!group_service_.getGroupMembers(conversation_id, &recipients, &error)) {
      sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                   "error", "GROUP_MEMBERS_FAILED", error, "");
      return;
    }
    recipients.erase(std::remove(recipients.begin(), recipients.end(), session->user_id), recipients.end());
    if (recipients.empty()) {
      sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                   "error", "NO_RECIPIENTS", "no recipients available", "");
      return;
    }
  } else {
    sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                 "error", "INVALID_CONVERSATION_TYPE", "use private or group", "");
    return;
  }

  MessageInput input;
  input.conversation_type = conversation_type;
  input.conversation_id = conversation_id;
  input.sender_id = session->user_id;
  input.sender_nickname = session->nickname;
  input.content = content;

  StoredMessage stored;
  if (!message_service_.storeMessage(input, recipients, &stored, &error)) {
    sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
                 "error", "STORE_FAILED", error, "");
    return;
  }

  nlohmann::json ack;
  ack["message_id"] = stored.message_id;
  ack["created_at"] = stored.created_at;
  sendResponse(conn, onlinetalk::common::PacketType::MessageSend, packet.header.request_id,
               "ok", "", "", ack.dump());

  nlohmann::json deliver_meta;
  deliver_meta["message_id"] = stored.message_id;
  deliver_meta["conversation_type"] = stored.conversation_type;
  deliver_meta["conversation_id"] = stored.conversation_id;
  deliver_meta["sender_id"] = stored.sender_id;
  deliver_meta["sender_nickname"] = stored.sender_nickname;
  deliver_meta["content"] = stored.content;
  deliver_meta["created_at"] = stored.created_at;
  const auto deliver_payload = deliver_meta.dump();

  for (const auto& user_id : recipients) {
    int fd = -1;
    if (!sessions_.tryGetFd(user_id, &fd)) {
      continue;
    }
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
      continue;
    }
    it->second->queueWrite(buildPacket(onlinetalk::common::PacketType::MessageDeliver,
                                       0,
                                       deliver_payload,
                                       nullptr));
    updateEpollEvents(fd, true);
    std::vector<int64_t> ids{stored.message_id};
    std::string mark_error;
    message_service_.markDelivered(user_id, ids, &mark_error);
  }
}

void TcpServer::handleFile(Connection& conn, const onlinetalk::common::Packet& packet) {
  const auto* session = sessions_.getSession(conn.fd());
  if (!session || !session->logged_in) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "NOT_LOGGED_IN",
                 "login required",
                 "");
    return;
  }

  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    sendResponse(conn,
                 static_cast<onlinetalk::common::PacketType>(packet.header.type),
                 packet.header.request_id,
                 "error",
                 "INVALID_JSON",
                 error,
                 "");
    return;
  }

  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  if (type == onlinetalk::common::PacketType::FileOffer) {
    const auto conversation_type = meta.value("conversation_type", "");
    const auto conversation_id = meta.value("conversation_id", "");
    const auto file_name = meta.value("file_name", "");
    const auto sha256 = meta.value("sha256", "");
    const auto file_id = meta.value("file_id", "");
    const auto file_size = meta.value("file_size", static_cast<int64_t>(0));

    if (!validateField(conversation_type, "conversation_type", kMaxFieldLength, &error) ||
        !validateField(conversation_id, "conversation_id", kMaxFieldLength, &error) ||
        !validateField(file_name, "file_name", kMaxFileNameLength, &error) ||
        !validateField(sha256, "sha256", kSha256HexLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_REQUEST", error, "");
      return;
    }
    if (sha256.size() != kSha256HexLength) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_SHA256", "sha256 length invalid", "");
      return;
    }
    if (file_size <= 0) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_SIZE", "file_size must be positive", "");
      return;
    }

    std::vector<std::string> recipients;
    if (conversation_type == "private") {
      std::string exists_error;
      const bool exists = auth_service_.userExists(conversation_id, &exists_error);
      if (!exists_error.empty()) {
        sendResponse(conn, type, packet.header.request_id, "error", "USER_LOOKUP_FAILED", exists_error, "");
        return;
      }
      if (!exists) {
        sendResponse(conn, type, packet.header.request_id, "error", "TARGET_NOT_FOUND", "target user not found", "");
        return;
      }
      recipients.push_back(conversation_id);
    } else if (conversation_type == "group") {
      std::string role;
      if (!group_service_.getUserRole(session->user_id, conversation_id, &role, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "NOT_IN_GROUP", error, "");
        return;
      }
      if (!group_service_.getGroupMembers(conversation_id, &recipients, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "GROUP_MEMBERS_FAILED", error, "");
        return;
      }
    } else {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_CONVERSATION_TYPE",
                   "use private or group", "");
      return;
    }

    UploadInfo info;
    if (!file_id.empty()) {
      if (!file_service_.resumeUpload(file_id, session->user_id, &info, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "RESUME_FAILED", error, "");
        return;
      }
    } else {
      FileOffer offer;
      offer.conversation_type = conversation_type;
      offer.conversation_id = conversation_id;
      offer.file_name = file_name;
      offer.file_size = file_size;
      offer.sha256 = sha256;
      offer.uploader_id = session->user_id;
      offer.uploader_nickname = session->nickname;
      offer.recipients = std::move(recipients);
      if (!file_service_.createUpload(offer, &info, &error)) {
        sendResponse(conn, type, packet.header.request_id, "error", "OFFER_FAILED", error, "");
        return;
      }
    }

    nlohmann::json response;
    response["file_id"] = info.file_id;
    response["next_offset"] = info.uploaded_size;
    response["chunk_size"] = file_service_.chunkSize();
    sendResponse(conn, onlinetalk::common::PacketType::FileAccept, packet.header.request_id,
                 "ok", "", "", response.dump());
    return;
  }

  if (type == onlinetalk::common::PacketType::FileUploadChunk) {
    const auto file_id = meta.value("file_id", "");
    const auto offset = meta.value("offset", static_cast<int64_t>(0));
    if (!validateField(file_id, "file_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_FILE_ID", error, "");
      return;
    }
    if (packet.binary.empty()) {
      sendResponse(conn, type, packet.header.request_id, "error", "EMPTY_CHUNK", "chunk is empty", "");
      return;
    }
    if (static_cast<int>(packet.binary.size()) > file_service_.chunkSize()) {
      sendResponse(conn, type, packet.header.request_id, "error", "CHUNK_TOO_LARGE", "chunk too large", "");
      return;
    }
    UploadInfo info;
    if (!file_service_.appendChunk(file_id, session->user_id, offset, packet.binary, &info, &error)) {
      nlohmann::json extra;
      if (error == "offset mismatch") {
        UploadInfo current;
        std::string resume_error;
        if (file_service_.resumeUpload(file_id, session->user_id, &current, &resume_error)) {
          extra["expected_offset"] = current.uploaded_size;
        }
      }
      sendResponse(conn, type, packet.header.request_id, "error", "UPLOAD_FAILED", error, extra.dump());
      return;
    }
    nlohmann::json extra;
    extra["next_offset"] = info.uploaded_size;
    sendResponse(conn, type, packet.header.request_id, "ok", "", "", extra.dump());
    return;
  }

  if (type == onlinetalk::common::PacketType::FileUploadDone) {
    const auto file_id = meta.value("file_id", "");
    if (!validateField(file_id, "file_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_FILE_ID", error, "");
      return;
    }
    FileNotice notice;
    if (!file_service_.finalizeUpload(file_id, session->user_id, &notice, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "FINALIZE_FAILED", error, "");
      return;
    }

    nlohmann::json done_meta;
    done_meta["file_id"] = notice.file_id;
    done_meta["conversation_type"] = notice.conversation_type;
    done_meta["conversation_id"] = notice.conversation_id;
    done_meta["file_name"] = notice.file_name;
    done_meta["file_size"] = notice.file_size;
    done_meta["sha256"] = notice.sha256;
    done_meta["uploader_id"] = notice.uploader_id;
    done_meta["uploader_nickname"] = notice.uploader_nickname;
    done_meta["created_at"] = notice.created_at;
    sendResponse(conn, onlinetalk::common::PacketType::FileDone, packet.header.request_id,
                 "ok", "", "", done_meta.dump());

    std::vector<std::string> targets;
    if (file_service_.listTargets(file_id, &targets, &error)) {
      std::vector<std::string> delivered;
      for (const auto& target : targets) {
        if (target == session->user_id) {
          delivered.push_back(target);
          continue;
        }
        int fd = -1;
        if (!sessions_.tryGetFd(target, &fd)) {
          continue;
        }
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
          continue;
        }
        it->second->queueWrite(buildPacket(onlinetalk::common::PacketType::FileDone, 0, done_meta.dump(), nullptr));
        updateEpollEvents(fd, true);
        delivered.push_back(target);
      }
      if (!delivered.empty()) {
        for (const auto& user_id : delivered) {
          std::string mark_error;
          file_service_.markDelivered(user_id, {file_id}, &mark_error);
        }
      }
    }
    return;
  }

  if (type == onlinetalk::common::PacketType::FileDownloadRequest) {
    const auto file_id = meta.value("file_id", "");
    const auto offset = meta.value("offset", static_cast<int64_t>(0));
    if (!validateField(file_id, "file_id", kMaxFieldLength, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "INVALID_FILE_ID", error, "");
      return;
    }
    std::vector<uint8_t> data;
    FileNotice notice;
    if (!file_service_.readChunk(file_id, session->user_id, offset, &data, &notice, &error)) {
      sendResponse(conn, type, packet.header.request_id, "error", "DOWNLOAD_FAILED", error, "");
      return;
    }
    const bool done = (offset + static_cast<int64_t>(data.size()) >= notice.file_size);
    nlohmann::json meta_resp;
    meta_resp["file_id"] = notice.file_id;
    meta_resp["offset"] = offset;
    meta_resp["file_size"] = notice.file_size;
    meta_resp["file_name"] = notice.file_name;
    meta_resp["sha256"] = notice.sha256;
    meta_resp["done"] = done;
    auto packet_out = buildPacket(onlinetalk::common::PacketType::FileDownloadChunk,
                                  packet.header.request_id,
                                  meta_resp.dump(),
                                  &data);
    conn.queueWrite(packet_out);
    updateEpollEvents(conn.fd(), true);
    return;
  }
}

void TcpServer::deliverOfflineMessages(const std::string& user_id, Connection& conn) {
  while (true) {
    std::vector<StoredMessage> messages;
    std::string error;
    const int batch = std::max(1, config_.history_page_size);
    if (!message_service_.fetchUndelivered(user_id, batch, &messages, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                      "fetch offline messages failed: " + error);
      return;
    }
    if (messages.empty()) {
      return;
    }

    std::vector<int64_t> delivered_ids;
    delivered_ids.reserve(messages.size());
    for (const auto& msg : messages) {
      nlohmann::json meta;
      meta["message_id"] = msg.message_id;
      meta["conversation_type"] = msg.conversation_type;
      meta["conversation_id"] = msg.conversation_id;
      meta["sender_id"] = msg.sender_id;
      meta["sender_nickname"] = msg.sender_nickname;
      meta["content"] = msg.content;
      meta["created_at"] = msg.created_at;
      conn.queueWrite(buildPacket(onlinetalk::common::PacketType::MessageDeliver, 0, meta.dump(), nullptr));
      delivered_ids.push_back(msg.message_id);
    }
    updateEpollEvents(conn.fd(), true);
    if (!message_service_.markDelivered(user_id, delivered_ids, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                      "mark offline delivered failed: " + error);
      return;
    }
  }
}

void TcpServer::deliverOfflineFiles(const std::string& user_id, Connection& conn) {
  while (true) {
    std::vector<FileNotice> notices;
    std::string error;
    const int batch = std::max(1, config_.history_page_size);
    if (!file_service_.fetchUndelivered(user_id, batch, &notices, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                      "fetch offline files failed: " + error);
      return;
    }
    if (notices.empty()) {
      return;
    }
    std::vector<std::string> delivered_ids;
    delivered_ids.reserve(notices.size());
    for (const auto& notice : notices) {
      nlohmann::json meta;
      meta["file_id"] = notice.file_id;
      meta["conversation_type"] = notice.conversation_type;
      meta["conversation_id"] = notice.conversation_id;
      meta["file_name"] = notice.file_name;
      meta["file_size"] = notice.file_size;
      meta["sha256"] = notice.sha256;
      meta["uploader_id"] = notice.uploader_id;
      meta["uploader_nickname"] = notice.uploader_nickname;
      meta["created_at"] = notice.created_at;
      conn.queueWrite(buildPacket(onlinetalk::common::PacketType::FileDone, 0, meta.dump(), nullptr));
      delivered_ids.push_back(notice.file_id);
    }
    updateEpollEvents(conn.fd(), true);
    if (!file_service_.markDelivered(user_id, delivered_ids, &error)) {
      onlinetalk::common::Logger::log(onlinetalk::common::LogLevel::Warn,
                                      "mark offline files delivered failed: " + error);
      return;
    }
  }
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
  updateEpollEvents(conn.fd(), true);
}

void TcpServer::sendAuthOk(Connection& conn, uint64_t request_id) {
  nlohmann::json meta;
  const auto* session = sessions_.getSession(conn.fd());
  if (session) {
    meta["user_id"] = session->user_id;
    meta["nickname"] = session->nickname;
  }
  meta["registered"] = false;
  meta["logged_in"] = true;
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
  updateEpollEvents(conn.fd(), true);
}

void TcpServer::sendResponse(Connection& conn,
                             onlinetalk::common::PacketType type,
                             uint64_t request_id,
                             const std::string& status,
                             const std::string& code,
                             const std::string& message,
                             const std::string& extra_meta_json) {
  nlohmann::json meta;
  if (!status.empty()) {
    meta["status"] = status;
  }
  if (!code.empty()) {
    meta["code"] = code;
  }
  if (!message.empty()) {
    meta["message"] = message;
  }
  if (!extra_meta_json.empty()) {
    nlohmann::json extra;
    std::string parse_error;
    if (parseJson(extra_meta_json, &extra, &parse_error)) {
      for (auto it = extra.begin(); it != extra.end(); ++it) {
        meta[it.key()] = it.value();
      }
    }
  }
  auto packet = buildPacket(type, request_id, meta.dump(), nullptr);
  conn.queueWrite(packet);
  updateEpollEvents(conn.fd(), true);
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

bool TcpServer::initDatabase(std::string* error) {
  if (!database_.open(config_.db_path, error)) {
    return false;
  }
  if (!database_.initSchema(error)) {
    return false;
  }
  return file_service_.ensureStorage(error);
}

}  // namespace onlinetalk::server
