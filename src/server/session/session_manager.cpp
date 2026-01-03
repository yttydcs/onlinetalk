#include "server/session/session_manager.h"

namespace onlinetalk::server {

void SessionManager::addConnection(int fd) {
  Session session;
  session.fd = fd;
  sessions_.emplace(fd, std::move(session));
}

void SessionManager::removeConnection(int fd) {
  auto it = sessions_.find(fd);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second.logged_in && !it->second.user_id.empty()) {
    user_to_fd_.erase(it->second.user_id);
  }
  sessions_.erase(it);
}

bool SessionManager::login(int fd, const std::string& user_id, const std::string& nickname, std::string* error) {
  auto it = sessions_.find(fd);
  if (it == sessions_.end()) {
    if (error) {
      *error = "session not found";
    }
    return false;
  }
  auto existing = user_to_fd_.find(user_id);
  if (existing != user_to_fd_.end() && existing->second != fd) {
    if (error) {
      *error = "user already online";
    }
    return false;
  }
  it->second.logged_in = true;
  it->second.user_id = user_id;
  it->second.nickname = nickname;
  user_to_fd_[user_id] = fd;
  return true;
}

void SessionManager::logout(int fd) {
  auto it = sessions_.find(fd);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second.logged_in && !it->second.user_id.empty()) {
    user_to_fd_.erase(it->second.user_id);
  }
  it->second.logged_in = false;
  it->second.user_id.clear();
  it->second.nickname.clear();
}

bool SessionManager::isLoggedIn(int fd) const {
  auto it = sessions_.find(fd);
  return it != sessions_.end() && it->second.logged_in;
}

std::vector<OnlineUser> SessionManager::onlineUsers() const {
  std::vector<OnlineUser> users;
  users.reserve(user_to_fd_.size());
  for (const auto& item : user_to_fd_) {
    auto it = sessions_.find(item.second);
    if (it == sessions_.end()) {
      continue;
    }
    OnlineUser user;
    user.user_id = it->second.user_id;
    user.nickname = it->second.nickname;
    users.push_back(std::move(user));
  }
  return users;
}

const Session* SessionManager::getSession(int fd) const {
  auto it = sessions_.find(fd);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return &it->second;
}

}  // namespace onlinetalk::server
