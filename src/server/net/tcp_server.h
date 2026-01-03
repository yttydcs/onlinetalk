#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/net/byte_buffer.h"
#include "common/protocol/packet.h"
#include "server/net/connection.h"
#include "server/session/session_manager.h"

namespace onlinetalk::server {

class TcpServer {
 public:
  explicit TcpServer(const onlinetalk::common::ServerConfig& config);
  ~TcpServer();

  bool start(std::string* error);
  void run();
  void stop();

 private:
  bool setupListener(std::string* error);
  void acceptConnections();
  bool handleRead(Connection& conn);
  bool handleWrite(Connection& conn);
  bool processPackets(Connection& conn);
  bool tryDecodePacket(Connection& conn, onlinetalk::common::Packet* packet, std::string* error);
  bool peekHeader(const onlinetalk::common::ByteBuffer& buffer,
                  onlinetalk::common::PacketHeader* header,
                  std::string* error) const;

  void handleAuth(Connection& conn, const onlinetalk::common::Packet& packet);
  void sendAuthError(Connection& conn,
                     uint64_t request_id,
                     const std::string& code,
                     const std::string& message);
  void sendAuthOk(Connection& conn, uint64_t request_id);
  void broadcastUserList();
  std::vector<uint8_t> buildPacket(onlinetalk::common::PacketType type,
                                   uint64_t request_id,
                                   const std::string& meta_json,
                                   const std::vector<uint8_t>* binary);
  void updateEpollEvents(int fd, bool want_write);
  void disconnect(int fd);

  onlinetalk::common::ServerConfig config_;
  int listen_fd_ = -1;
  int epoll_fd_ = -1;
  bool running_ = false;
  SessionManager sessions_;
  std::unordered_map<int, std::unique_ptr<Connection>> connections_;
};

}  // namespace onlinetalk::server
