#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/storage/database.h"

namespace onlinetalk::server {

struct MessageInput {
  std::string conversation_type;
  std::string conversation_id;
  std::string sender_id;
  std::string sender_nickname;
  std::string content;
};

struct StoredMessage {
  int64_t message_id = 0;
  std::string conversation_type;
  std::string conversation_id;
  std::string sender_id;
  std::string sender_nickname;
  std::string content;
  int64_t created_at = 0;
};

class MessageService {
 public:
  explicit MessageService(Database& db);

  bool storeMessage(const MessageInput& input,
                    const std::vector<std::string>& recipients,
                    StoredMessage* stored,
                    std::string* error);
  bool fetchUndelivered(const std::string& user_id,
                        int limit,
                        std::vector<StoredMessage>* out,
                        std::string* error);
  bool markDelivered(const std::string& user_id,
                     const std::vector<int64_t>& message_ids,
                     std::string* error);
  bool fetchHistory(const std::string& user_id,
                    const std::string& conversation_type,
                    const std::string& conversation_id,
                    int64_t before_message_id,
                    int limit,
                    std::vector<StoredMessage>* out,
                    std::string* error);

 private:
  Database& db_;
};

}  // namespace onlinetalk::server
