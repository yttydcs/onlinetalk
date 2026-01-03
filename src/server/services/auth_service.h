#pragma once

#include <string>

#include "server/storage/database.h"

namespace onlinetalk::server {

struct AuthUser {
  std::string user_id;
  std::string nickname;
};

class AuthService {
 public:
  explicit AuthService(Database& db);

  bool registerUser(const std::string& user_id,
                    const std::string& nickname,
                    const std::string& password,
                    std::string* error);
  bool loginUser(const std::string& user_id,
                 const std::string& password,
                 AuthUser* user,
                 std::string* error);
  bool userExists(const std::string& user_id, std::string* error);

 private:
  std::string hashPassword(const std::string& password, std::string* error);
  bool verifyPassword(const std::string& password, const std::string& hash);

  Database& db_;
};

}  // namespace onlinetalk::server
