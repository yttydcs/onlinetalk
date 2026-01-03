#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace onlinetalk::server {

struct Session {
  int fd = -1;
  bool logged_in = false;
  std::string user_id;
  std::string nickname;
};

struct OnlineUser {
  std::string user_id;
  std::string nickname;
};

class SessionManager {
 public:
  void addConnection(int fd);
  void removeConnection(int fd);
  bool login(int fd, const std::string& user_id, const std::string& nickname, std::string* error);
  void logout(int fd);
  bool isLoggedIn(int fd) const;
  std::vector<OnlineUser> onlineUsers() const;
  const Session* getSession(int fd) const;

 private:
  std::unordered_map<int, Session> sessions_;
  std::unordered_map<std::string, int> user_to_fd_;
};

}  // namespace onlinetalk::server
