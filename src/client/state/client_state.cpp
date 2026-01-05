#include "client/state/client_state.h"

#include <algorithm>

namespace onlinetalk::client {

namespace {

UserSummary parseUser(const nlohmann::json& item) {
  UserSummary user;
  user.user_id = item.value("user_id", "");
  user.nickname = item.value("nickname", "");
  return user;
}

MessageRecord parseMessage(const nlohmann::json& item,
                           const std::string& conversation_type,
                           const std::string& conversation_id) {
  MessageRecord record;
  record.message_id = item.value("message_id", static_cast<int64_t>(0));
  record.conversation_type = conversation_type;
  record.conversation_id = conversation_id;
  record.sender_id = item.value("sender_id", "");
  record.sender_nickname = item.value("sender_nickname", "");
  record.content = item.value("content", "");
  record.created_at = item.value("created_at", static_cast<int64_t>(0));
  return record;
}

FileNotice parseFileNotice(const nlohmann::json& item) {
  FileNotice notice;
  notice.file_id = item.value("file_id", "");
  notice.conversation_type = item.value("conversation_type", "");
  notice.conversation_id = item.value("conversation_id", "");
  notice.file_name = item.value("file_name", "");
  notice.file_size = item.value("file_size", static_cast<int64_t>(0));
  notice.sha256 = item.value("sha256", "");
  notice.uploader_id = item.value("uploader_id", "");
  notice.uploader_nickname = item.value("uploader_nickname", "");
  notice.created_at = item.value("created_at", static_cast<int64_t>(0));
  return notice;
}

}  // namespace

bool ClientState::loggedIn() const {
  return logged_in_;
}

const std::string& ClientState::userId() const {
  return user_id_;
}

const std::string& ClientState::nickname() const {
  return nickname_;
}

const std::vector<UserSummary>& ClientState::onlineUsers() const {
  return online_users_;
}

const std::string& ClientState::lastError() const {
  return last_error_;
}

ConversationState* ClientState::getConversation(const std::string& conversation_type,
                                                const std::string& conversation_id) {
  const auto key = conversationKey(conversation_type, conversation_id);
  auto it = conversations_.find(key);
  if (it == conversations_.end()) {
    return nullptr;
  }
  return &it->second;
}

const ConversationState* ClientState::getConversation(const std::string& conversation_type,
                                                      const std::string& conversation_id) const {
  const auto key = conversationKey(conversation_type, conversation_id);
  auto it = conversations_.find(key);
  if (it == conversations_.end()) {
    return nullptr;
  }
  return &it->second;
}

void ClientState::applyPacket(const onlinetalk::common::Packet& packet) {
  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    last_error_ = error;
    return;
  }

  switch (type) {
    case onlinetalk::common::PacketType::AuthOk:
      applyAuthOk(meta);
      break;
    case onlinetalk::common::PacketType::AuthError:
      applyAuthError(meta);
      break;
    case onlinetalk::common::PacketType::UserListUpdate:
      applyUserList(meta);
      break;
    case onlinetalk::common::PacketType::MessageDeliver:
      applyMessageDeliver(meta);
      break;
    case onlinetalk::common::PacketType::HistoryResponse:
      applyHistoryResponse(meta);
      break;
    case onlinetalk::common::PacketType::FileDone:
      applyFileNotice(meta);
      break;
    default:
      break;
  }
}

int64_t ClientState::nextHistoryBeforeId(const std::string& conversation_type,
                                         const std::string& conversation_id) const {
  return history_manager_.nextBeforeId(conversationKey(conversation_type, conversation_id));
}

bool ClientState::hasMoreHistory(const std::string& conversation_type,
                                 const std::string& conversation_id) const {
  return history_manager_.hasMore(conversationKey(conversation_type, conversation_id));
}

void ClientState::resetHistoryCursor(const std::string& conversation_type,
                                     const std::string& conversation_id) {
  history_manager_.reset(conversationKey(conversation_type, conversation_id));
}

std::string ClientState::conversationKey(const std::string& type, const std::string& id) {
  return type + ":" + id;
}

bool ClientState::parseJson(const std::string& text, nlohmann::json* out, std::string* error) {
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

ConversationState& ClientState::ensureConversation(const std::string& conversation_type,
                                                   const std::string& conversation_id) {
  const auto key = conversationKey(conversation_type, conversation_id);
  auto it = conversations_.find(key);
  if (it != conversations_.end()) {
    return it->second;
  }
  ConversationState state;
  state.conversation_type = conversation_type;
  state.conversation_id = conversation_id;
  auto inserted = conversations_.emplace(key, std::move(state));
  return inserted.first->second;
}

void ClientState::applyAuthOk(const nlohmann::json& meta) {
  logged_in_ = meta.value("logged_in", false);
  user_id_ = meta.value("user_id", "");
  nickname_ = meta.value("nickname", "");

  online_users_.clear();
  if (meta.contains("online_users") && meta["online_users"].is_array()) {
    for (const auto& item : meta["online_users"]) {
      online_users_.push_back(parseUser(item));
    }
  }
}

void ClientState::applyAuthError(const nlohmann::json& meta) {
  const auto code = meta.value("code", "");
  const auto message = meta.value("message", "");
  if (!code.empty()) {
    last_error_ = code + ": " + message;
  } else {
    last_error_ = message;
  }
}

void ClientState::applyUserList(const nlohmann::json& meta) {
  online_users_.clear();
  if (!meta.contains("users") || !meta["users"].is_array()) {
    return;
  }
  for (const auto& item : meta["users"]) {
    online_users_.push_back(parseUser(item));
  }
}

void ClientState::applyMessageDeliver(const nlohmann::json& meta) {
  const auto conversation_type = meta.value("conversation_type", "");
  const auto conversation_id = meta.value("conversation_id", "");
  if (conversation_type.empty() || conversation_id.empty()) {
    return;
  }
  auto& conversation = ensureConversation(conversation_type, conversation_id);
  MessageRecord record = parseMessage(meta, conversation_type, conversation_id);
  conversation.messages.push_back(std::move(record));
}

void ClientState::applyHistoryResponse(const nlohmann::json& meta) {
  const auto status = meta.value("status", "");
  if (!status.empty() && status != "ok") {
    const auto code = meta.value("code", "");
    const auto message = meta.value("message", "");
    last_error_ = code.empty() ? message : code + ": " + message;
    return;
  }
  const auto conversation_type = meta.value("conversation_type", "");
  const auto conversation_id = meta.value("conversation_id", "");
  if (conversation_type.empty() || conversation_id.empty()) {
    return;
  }

  const auto key = conversationKey(conversation_type, conversation_id);
  auto& conversation = ensureConversation(conversation_type, conversation_id);
  std::vector<MessageRecord> batch;
  if (meta.contains("messages") && meta["messages"].is_array()) {
    for (const auto& item : meta["messages"]) {
      batch.push_back(parseMessage(item, conversation_type, conversation_id));
    }
  }

  if (conversation.messages.empty()) {
    for (auto& msg : batch) {
      conversation.messages.push_back(std::move(msg));
    }
  } else if (!batch.empty() && batch.back().message_id < conversation.messages.front().message_id) {
    for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
      conversation.messages.push_front(*it);
    }
  } else {
    for (auto& msg : batch) {
      conversation.messages.push_back(std::move(msg));
    }
  }

  const auto next_before = meta.value("next_before_message_id", static_cast<int64_t>(0));
  const auto count = static_cast<size_t>(meta.value("count", 0));
  history_manager_.update(key, next_before, count);
}

void ClientState::applyFileNotice(const nlohmann::json& meta) {
  FileNotice notice = parseFileNotice(meta);
  if (notice.file_id.empty()) {
    return;
  }
  auto& conversation = ensureConversation(notice.conversation_type, notice.conversation_id);
  conversation.files.push_back(std::move(notice));
}

}  // namespace onlinetalk::client
