#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "client/history/history_manager.h"
#include "common/protocol/packet.h"

namespace onlinetalk::client {

struct UserSummary {
  std::string user_id;
  std::string nickname;
};

struct MessageRecord {
  int64_t message_id = 0;
  std::string conversation_type;
  std::string conversation_id;
  std::string sender_id;
  std::string sender_nickname;
  std::string content;
  int64_t created_at = 0;
};

struct FileNotice {
  std::string file_id;
  std::string conversation_type;
  std::string conversation_id;
  std::string file_name;
  int64_t file_size = 0;
  std::string sha256;
  std::string uploader_id;
  std::string uploader_nickname;
  int64_t created_at = 0;
};

struct ConversationState {
  std::string conversation_type;
  std::string conversation_id;
  std::deque<MessageRecord> messages;
  std::deque<FileNotice> files;
};

class ClientState {
 public:
  bool loggedIn() const;
  const std::string& userId() const;
  const std::string& nickname() const;
  const std::vector<UserSummary>& onlineUsers() const;
  const std::string& lastError() const;

  ConversationState* getConversation(const std::string& conversation_type,
                                      const std::string& conversation_id);
  const ConversationState* getConversation(const std::string& conversation_type,
                                           const std::string& conversation_id) const;

  void applyPacket(const onlinetalk::common::Packet& packet);
  int64_t nextHistoryBeforeId(const std::string& conversation_type,
                              const std::string& conversation_id) const;
  bool hasMoreHistory(const std::string& conversation_type,
                      const std::string& conversation_id) const;
  void resetHistoryCursor(const std::string& conversation_type,
                          const std::string& conversation_id);

 private:
  static std::string conversationKey(const std::string& type, const std::string& id);
  static bool parseJson(const std::string& text, nlohmann::json* out, std::string* error);

  void applyAuthOk(const nlohmann::json& meta);
  void applyAuthError(const nlohmann::json& meta);
  void applyUserList(const nlohmann::json& meta);
  void applyMessageDeliver(const nlohmann::json& meta);
  void applyHistoryResponse(const nlohmann::json& meta);
  void applyFileNotice(const nlohmann::json& meta);

  ConversationState& ensureConversation(const std::string& conversation_type,
                                        const std::string& conversation_id);

  bool logged_in_ = false;
  std::string user_id_;
  std::string nickname_;
  std::vector<UserSummary> online_users_;
  std::string last_error_;
  std::unordered_map<std::string, ConversationState> conversations_;
  HistoryManager history_manager_;
};

}  // namespace onlinetalk::client
