#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/net/byte_buffer.h"
#include "common/protocol/codec.h"
#include "common/protocol/packet.h"

namespace onlinetalk::client {

class NetClient {
 public:
  NetClient();
  ~NetClient();

  bool connectTo(const std::string& host, uint16_t port, std::string* error);
  void start();
  void stop();
  bool isRunning() const;

  uint64_t nextRequestId();
  bool sendPacket(onlinetalk::common::PacketType type,
                  uint64_t request_id,
                  const std::string& meta_json,
                  const std::vector<uint8_t>* binary);
  bool sendJson(onlinetalk::common::PacketType type,
                uint64_t request_id,
                const nlohmann::json& meta,
                const std::vector<uint8_t>* binary);

  bool pollPacket(onlinetalk::common::Packet* out);
  std::string lastError() const;

 private:
  void runLoop();
  bool flushWrite(std::string* error);
  bool readAvailable(std::string* error);
  void queuePacket(onlinetalk::common::Packet&& packet);
  void setLastError(const std::string& error);

  int socket_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread io_thread_;

  std::atomic<uint64_t> next_request_id_{1};

  mutable std::mutex error_mutex_;
  std::string last_error_;

  std::mutex write_mutex_;
  std::vector<uint8_t> write_buffer_;
  size_t write_offset_ = 0;

  std::mutex read_mutex_;
  std::deque<onlinetalk::common::Packet> incoming_;
  onlinetalk::common::ByteBuffer read_buffer_;
};

}  // namespace onlinetalk::client
