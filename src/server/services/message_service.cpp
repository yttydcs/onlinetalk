#include "server/services/message_service.h"

#include <chrono>

namespace onlinetalk::server {

namespace {

int64_t nowSeconds() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string textOrEmpty(sqlite3_stmt* stmt, int col) {
  const auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
  return text ? text : "";
}

}  // namespace

MessageService::MessageService(Database& db) : db_(db) {}

bool MessageService::storeMessage(const MessageInput& input,
                                  const std::vector<std::string>& recipients,
                                  StoredMessage* stored,
                                  std::string* error) {
  if (!stored) {
    if (error) {
      *error = "stored output is null";
    }
    return false;
  }
  if (recipients.empty()) {
    if (error) {
      *error = "recipients empty";
    }
    return false;
  }
  if (!db_.execute("BEGIN;", error)) {
    return false;
  }

  bool ok = false;
  do {
    const std::string insert_message =
        "INSERT INTO messages(conversation_type, conversation_id, sender_id, sender_nickname, content, created_at) "
        "VALUES(?,?,?,?,?,?);";
    Statement stmt(db_.handle(), insert_message, error);
    if (!stmt.valid()) {
      break;
    }
    sqlite3_bind_text(stmt.get(), 1, input.conversation_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, input.conversation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, input.sender_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, input.sender_nickname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, input.content.c_str(), -1, SQLITE_TRANSIENT);
    const auto created_at = nowSeconds();
    sqlite3_bind_int64(stmt.get(), 6, created_at);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const auto message_id = sqlite3_last_insert_rowid(db_.handle());
    const std::string insert_target =
        "INSERT INTO message_targets(message_id, user_id, delivered_at) VALUES(?,?,NULL);";
    Statement target_stmt(db_.handle(), insert_target, error);
    if (!target_stmt.valid()) {
      break;
    }
    bool targets_ok = true;
    for (const auto& user_id : recipients) {
      sqlite3_reset(target_stmt.get());
      sqlite3_clear_bindings(target_stmt.get());
      sqlite3_bind_int64(target_stmt.get(), 1, message_id);
      sqlite3_bind_text(target_stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(target_stmt.get()) != SQLITE_DONE) {
        if (error) {
          *error = sqlite3_errmsg(db_.handle());
        }
        targets_ok = false;
        break;
      }
    }
    if (!targets_ok) {
      break;
    }

    stored->message_id = message_id;
    stored->conversation_type = input.conversation_type;
    stored->conversation_id = input.conversation_id;
    stored->sender_id = input.sender_id;
    stored->sender_nickname = input.sender_nickname;
    stored->content = input.content;
    stored->created_at = created_at;
    ok = true;
  } while (false);

  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  return ok;
}

bool MessageService::fetchUndelivered(const std::string& user_id,
                                      int limit,
                                      std::vector<StoredMessage>* out,
                                      std::string* error) {
  if (!out) {
    if (error) {
      *error = "output list is null";
    }
    return false;
  }
  out->clear();
  const std::string sql =
      "SELECT m.message_id, m.conversation_type, m.conversation_id, m.sender_id, "
      "m.sender_nickname, m.content, m.created_at "
      "FROM message_targets t "
      "JOIN messages m ON t.message_id = m.message_id "
      "WHERE t.user_id = ? AND t.delivered_at IS NULL "
      "ORDER BY m.message_id ASC LIMIT ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 2, limit);
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      StoredMessage msg;
      msg.message_id = sqlite3_column_int64(stmt.get(), 0);
      msg.conversation_type = textOrEmpty(stmt.get(), 1);
      msg.conversation_id = textOrEmpty(stmt.get(), 2);
      msg.sender_id = textOrEmpty(stmt.get(), 3);
      msg.sender_nickname = textOrEmpty(stmt.get(), 4);
      msg.content = textOrEmpty(stmt.get(), 5);
      msg.created_at = sqlite3_column_int64(stmt.get(), 6);
      out->push_back(std::move(msg));
      continue;
    }
    if (rc == SQLITE_DONE) {
      break;
    }
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool MessageService::markDelivered(const std::string& user_id,
                                   const std::vector<int64_t>& message_ids,
                                   std::string* error) {
  if (message_ids.empty()) {
    return true;
  }
  if (!db_.execute("BEGIN;", error)) {
    return false;
  }
  bool ok = false;
  do {
    const std::string sql =
        "UPDATE message_targets SET delivered_at = ? WHERE user_id = ? AND message_id = ?;";
    Statement stmt(db_.handle(), sql, error);
    if (!stmt.valid()) {
      break;
    }
    const auto delivered_at = nowSeconds();
    for (const auto message_id : message_ids) {
      sqlite3_reset(stmt.get());
      sqlite3_clear_bindings(stmt.get());
      sqlite3_bind_int64(stmt.get(), 1, delivered_at);
      sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt.get(), 3, message_id);
      if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        if (error) {
          *error = sqlite3_errmsg(db_.handle());
        }
        break;
      }
    }
    if (error && !error->empty()) {
      break;
    }
    ok = true;
  } while (false);

  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  return ok;
}

}  // namespace onlinetalk::server
