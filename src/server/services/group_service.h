#pragma once

#include <string>
#include <vector>

#include "server/storage/database.h"

namespace onlinetalk::server {

struct GroupInfo {
  std::string group_id;
  std::string name;
  std::string owner_id;
};

class GroupService {
 public:
  explicit GroupService(Database& db);

  bool createGroup(const std::string& owner_id,
                   const std::string& name,
                   std::string* group_id,
                   std::string* error);
  bool joinGroup(const std::string& user_id, const std::string& group_id, std::string* error);
  bool leaveGroup(const std::string& user_id, const std::string& group_id, std::string* error);
  bool renameGroup(const std::string& actor_id,
                   const std::string& group_id,
                   const std::string& new_name,
                   std::string* error);
  bool kickUser(const std::string& actor_id,
                const std::string& group_id,
                const std::string& target_user_id,
                std::string* error);
  bool dissolveGroup(const std::string& actor_id, const std::string& group_id, std::string* error);
  bool setAdmin(const std::string& actor_id,
                const std::string& group_id,
                const std::string& target_user_id,
                bool make_admin,
                std::string* error);
  bool getGroupMembers(const std::string& group_id, std::vector<std::string>* members, std::string* error);
  bool getUserRole(const std::string& user_id, const std::string& group_id, std::string* role, std::string* error);

 private:
  bool groupExists(const std::string& group_id, GroupInfo* info, std::string* error);
  bool isOwnerOrAdmin(const std::string& user_id, const std::string& group_id, bool* is_owner, std::string* error);

  Database& db_;
};

}  // namespace onlinetalk::server
