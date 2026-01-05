#include "client/net/client_api.h"

namespace onlinetalk::client {

ClientApi::ClientApi(NetClient& net) : net_(net) {}

uint64_t ClientApi::sendRegister(const std::string& user_id,
                                 const std::string& nickname,
                                 const std::string& password,
                                 std::string* error) {
  nlohmann::json meta;
  meta["user_id"] = user_id;
  meta["nickname"] = nickname;
  meta["password"] = password;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::AuthRegister, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::sendLogin(const std::string& user_id,
                              const std::string& password,
                              std::string* error) {
  nlohmann::json meta;
  meta["user_id"] = user_id;
  meta["password"] = password;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::AuthLogin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::sendMessage(const std::string& conversation_type,
                                const std::string& conversation_id,
                                const std::string& content,
                                std::string* error) {
  nlohmann::json meta;
  meta["conversation_type"] = conversation_type;
  meta["conversation_id"] = conversation_id;
  meta["content"] = content;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::MessageSend, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::fetchHistory(const std::string& conversation_type,
                                 const std::string& conversation_id,
                                 int64_t before_message_id,
                                 int limit,
                                 std::string* error) {
  nlohmann::json meta;
  meta["conversation_type"] = conversation_type;
  meta["conversation_id"] = conversation_id;
  meta["before_message_id"] = before_message_id;
  meta["limit"] = limit;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::HistoryFetch, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::createGroup(const std::string& name, std::string* error) {
  nlohmann::json meta;
  meta["name"] = name;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupCreate, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::joinGroup(const std::string& group_id, std::string* error) {
  nlohmann::json meta;
  meta["group_id"] = group_id;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupJoin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::leaveGroup(const std::string& group_id, std::string* error) {
  nlohmann::json meta;
  meta["group_id"] = group_id;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupLeave, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::renameGroup(const std::string& group_id, const std::string& name, std::string* error) {
  nlohmann::json meta;
  meta["action"] = "rename";
  meta["group_id"] = group_id;
  meta["name"] = name;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupAdmin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::kickFromGroup(const std::string& group_id,
                                  const std::string& target_user_id,
                                  std::string* error) {
  nlohmann::json meta;
  meta["action"] = "kick";
  meta["group_id"] = group_id;
  meta["target_user_id"] = target_user_id;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupAdmin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::dissolveGroup(const std::string& group_id, std::string* error) {
  nlohmann::json meta;
  meta["action"] = "dissolve";
  meta["group_id"] = group_id;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupAdmin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

uint64_t ClientApi::setGroupAdmin(const std::string& group_id,
                                  const std::string& target_user_id,
                                  bool make_admin,
                                  std::string* error) {
  nlohmann::json meta;
  meta["action"] = make_admin ? "promote" : "demote";
  meta["group_id"] = group_id;
  meta["target_user_id"] = target_user_id;
  uint64_t request_id = 0;
  if (!sendJson(onlinetalk::common::PacketType::GroupAdmin, meta, &request_id, error)) {
    return 0;
  }
  return request_id;
}

bool ClientApi::sendJson(onlinetalk::common::PacketType type,
                         const nlohmann::json& meta,
                         uint64_t* request_id,
                         std::string* error) {
  const auto req_id = net_.nextRequestId();
  if (!net_.sendJson(type, req_id, meta, nullptr)) {
    if (error) {
      *error = "send failed";
    }
    return false;
  }
  if (request_id) {
    *request_id = req_id;
  }
  return true;
}

}  // namespace onlinetalk::client
