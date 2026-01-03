#include "server/storage/database.h"

#include <utility>

#include "common/log.h"

namespace onlinetalk::server {

Statement::Statement(sqlite3* db, const std::string& sql, std::string* error) {
  if (!db) {
    if (error) {
      *error = "db is null";
    }
    return;
  }
  if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK) {
    if (error) {
      *error = sqlite3_errmsg(db);
    }
    stmt_ = nullptr;
  }
}

Statement::~Statement() {
  if (stmt_) {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
  }
}

sqlite3_stmt* Statement::get() const {
  return stmt_;
}

bool Statement::valid() const {
  return stmt_ != nullptr;
}

Database::~Database() {
  close();
}

bool Database::open(const std::string& path, std::string* error) {
  if (db_) {
    return true;
  }
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    if (error) {
      *error = sqlite3_errmsg(db_);
    }
    close();
    return false;
  }
  sqlite3_busy_timeout(db_, 3000);
  return true;
}

void Database::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

sqlite3* Database::handle() const {
  return db_;
}

bool Database::execute(const std::string& sql, std::string* error) {
  if (!db_) {
    if (error) {
      *error = "db is not open";
    }
    return false;
  }
  char* message = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    if (error) {
      *error = message ? message : "sqlite exec failed";
    }
    if (message) {
      sqlite3_free(message);
    }
    return false;
  }
  return true;
}

bool Database::initSchema(std::string* error) {
  if (!execute("PRAGMA journal_mode=WAL;", error)) {
    return false;
  }
  if (!execute("PRAGMA foreign_keys=ON;", error)) {
    return false;
  }

  const char* schema_sql = R"SQL(
CREATE TABLE IF NOT EXISTS users (
  user_id TEXT PRIMARY KEY,
  nickname TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS groups (
  group_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  owner_id TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS group_members (
  group_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  role TEXT NOT NULL,
  joined_at INTEGER NOT NULL,
  PRIMARY KEY (group_id, user_id)
);

CREATE TABLE IF NOT EXISTS messages (
  message_id INTEGER PRIMARY KEY AUTOINCREMENT,
  conversation_type TEXT NOT NULL,
  conversation_id TEXT NOT NULL,
  sender_id TEXT NOT NULL,
  sender_nickname TEXT NOT NULL,
  content TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS message_targets (
  message_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  delivered_at INTEGER,
  PRIMARY KEY (message_id, user_id)
);

CREATE TABLE IF NOT EXISTS files (
  file_id TEXT PRIMARY KEY,
  uploader_id TEXT NOT NULL,
  uploader_nickname TEXT NOT NULL,
  conversation_type TEXT NOT NULL,
  conversation_id TEXT NOT NULL,
  file_name TEXT NOT NULL,
  file_size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  storage_path TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS file_uploads (
  file_id TEXT PRIMARY KEY,
  uploader_id TEXT NOT NULL,
  temp_path TEXT NOT NULL,
  uploaded_size INTEGER NOT NULL,
  status TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS file_targets (
  file_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  delivered_at INTEGER,
  PRIMARY KEY (file_id, user_id)
);

CREATE INDEX IF NOT EXISTS idx_group_members_user ON group_members(user_id);
CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_type, conversation_id);
CREATE INDEX IF NOT EXISTS idx_targets_user ON message_targets(user_id, delivered_at);
CREATE INDEX IF NOT EXISTS idx_files_conversation ON files(conversation_type, conversation_id);
CREATE INDEX IF NOT EXISTS idx_file_targets_user ON file_targets(user_id, delivered_at);
)SQL";

  if (!execute(schema_sql, error)) {
    return false;
  }

  auto addColumnIfMissing = [&](const std::string& sql) -> bool {
    std::string local_error;
    if (!execute(sql, &local_error)) {
      if (local_error.find("duplicate column name") != std::string::npos) {
        return true;
      }
      if (error) {
        *error = local_error;
      }
      return false;
    }
    return true;
  };

  if (!addColumnIfMissing("ALTER TABLE files ADD COLUMN uploader_nickname TEXT NOT NULL DEFAULT '';")) {
    return false;
  }
  return true;
}

}  // namespace onlinetalk::server
