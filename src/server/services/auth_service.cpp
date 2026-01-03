#include "server/services/auth_service.h"

#include <chrono>
#include <crypt.h>

namespace onlinetalk::server {

namespace {

int64_t nowSeconds() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

}  // namespace

AuthService::AuthService(Database& db) : db_(db) {}

bool AuthService::registerUser(const std::string& user_id,
                               const std::string& nickname,
                               const std::string& password,
                               std::string* error) {
  if (user_id.empty() || nickname.empty() || password.empty()) {
    if (error) {
      *error = "user_id, nickname, password are required";
    }
    return false;
  }

  std::string exists_error;
  const bool exists = userExists(user_id, &exists_error);
  if (!exists_error.empty()) {
    if (error) {
      *error = exists_error;
    }
    return false;
  }
  if (exists) {
    if (error) {
      *error = "user already exists";
    }
    return false;
  }

  const auto hash = hashPassword(password, error);
  if (hash.empty()) {
    return false;
  }

  const std::string sql =
      "INSERT INTO users(user_id, nickname, password_hash, created_at) VALUES(?,?,?,?);";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, nickname.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 4, nowSeconds());
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool AuthService::loginUser(const std::string& user_id,
                            const std::string& password,
                            AuthUser* user,
                            std::string* error) {
  if (!user) {
    if (error) {
      *error = "user output is null";
    }
    return false;
  }
  const std::string sql =
      "SELECT nickname, password_hash FROM users WHERE user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    const auto nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const auto password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    if (!nickname || !password_hash) {
      if (error) {
        *error = "user record is invalid";
      }
      return false;
    }
    if (!verifyPassword(password, password_hash)) {
      if (error) {
        *error = "password mismatch";
      }
      return false;
    }
    user->user_id = user_id;
    user->nickname = nickname;
    return true;
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "user not found";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool AuthService::userExists(const std::string& user_id, std::string* error) {
  const std::string sql = "SELECT 1 FROM users WHERE user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

std::string AuthService::hashPassword(const std::string& password, std::string* error) {
  constexpr int kRounds = 12;
  const auto salt = crypt_gensalt("$2b$", kRounds, nullptr, 0);
  if (!salt) {
    if (error) {
      *error = "failed to generate bcrypt salt";
    }
    return {};
  }
  const auto hashed = crypt(password.c_str(), salt);
  if (!hashed) {
    if (error) {
      *error = "failed to hash password";
    }
    return {};
  }
  return hashed;
}

bool AuthService::verifyPassword(const std::string& password, const std::string& hash) {
  const auto hashed = crypt(password.c_str(), hash.c_str());
  if (!hashed) {
    return false;
  }
  return hash == hashed;
}

}  // namespace onlinetalk::server
