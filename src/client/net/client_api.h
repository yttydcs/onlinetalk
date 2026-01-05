#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "client/net/net_client.h"

namespace onlinetalk::client {

class ClientApi {
 public:
  explicit ClientApi(NetClient& net);

  uint64_t sendRegister(const std::string& user_id,
                        const std::string& nickname,
                        const std::string& password,
                        std::string* error);
  uint64_t sendLogin(const std::string& user_id,
                     const std::string& password,
                     std::string* error);
  uint64_t sendMessage(const std::string& conversation_type,
                       const std::string& conversation_id,
                       const std::string& content,
                       std::string* error);
  uint64_t fetchHistory(const std::string& conversation_type,
                        const std::string& conversation_id,
                        int64_t before_message_id,
                        int limit,
                        std::string* error);
  uint64_t createGroup(const std::string& name, std::string* error);
  uint64_t joinGroup(const std::string& group_id, std::string* error);
  uint64_t leaveGroup(const std::string& group_id, std::string* error);
  uint64_t renameGroup(const std::string& group_id, const std::string& name, std::string* error);
  uint64_t kickFromGroup(const std::string& group_id, const std::string& target_user_id, std::string* error);
  uint64_t dissolveGroup(const std::string& group_id, std::string* error);
  uint64_t setGroupAdmin(const std::string& group_id,
                         const std::string& target_user_id,
                         bool make_admin,
                         std::string* error);

 private:
  bool sendJson(onlinetalk::common::PacketType type,
                const nlohmann::json& meta,
                uint64_t* request_id,
                std::string* error);

  NetClient& net_;
};

}  // namespace onlinetalk::client
