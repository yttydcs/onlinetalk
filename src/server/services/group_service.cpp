#include "server/services/group_service.h"

#include <chrono>

#include "server/services/id_generator.h"

namespace onlinetalk::server {

namespace {

int64_t nowSeconds() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

}  // namespace

GroupService::GroupService(Database& db) : db_(db) {}

bool GroupService::createGroup(const std::string& owner_id,
                               const std::string& name,
                               std::string* group_id,
                               std::string* error) {
  if (!group_id) {
    if (error) {
      *error = "group_id output is null";
    }
    return false;
  }
  if (owner_id.empty() || name.empty()) {
    if (error) {
      *error = "owner_id and name are required";
    }
    return false;
  }

  *group_id = generateId();
  if (!db_.execute("BEGIN;", error)) {
    return false;
  }

  bool ok = false;
  do {
    const std::string insert_group =
        "INSERT INTO groups(group_id, name, owner_id, created_at) VALUES(?,?,?,?);";
    Statement group_stmt(db_.handle(), insert_group, error);
    if (!group_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(group_stmt.get(), 1, group_id->c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(group_stmt.get(), 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(group_stmt.get(), 3, owner_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(group_stmt.get(), 4, nowSeconds());
    if (sqlite3_step(group_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string insert_member =
        "INSERT INTO group_members(group_id, user_id, role, joined_at) VALUES(?,?,?,?);";
    Statement member_stmt(db_.handle(), insert_member, error);
    if (!member_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(member_stmt.get(), 1, group_id->c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(member_stmt.get(), 2, owner_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(member_stmt.get(), 3, "owner", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(member_stmt.get(), 4, nowSeconds());
    if (sqlite3_step(member_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    ok = true;
  } while (false);

  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  return ok;
}

bool GroupService::joinGroup(const std::string& user_id, const std::string& group_id, std::string* error) {
  GroupInfo info;
  if (!groupExists(group_id, &info, error)) {
    if (error && error->empty()) {
      *error = "group not found";
    }
    return false;
  }

  std::string role;
  std::string role_error;
  if (getUserRole(user_id, group_id, &role, &role_error)) {
    if (error) {
      *error = "user already in group";
    }
    return false;
  }
  if (!role_error.empty() && role_error != "user not in group") {
    if (error) {
      *error = role_error;
    }
    return false;
  }

  const std::string insert_member =
      "INSERT INTO group_members(group_id, user_id, role, joined_at) VALUES(?,?,?,?);";
  Statement stmt(db_.handle(), insert_member, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, "member", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 4, nowSeconds());
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool GroupService::leaveGroup(const std::string& user_id, const std::string& group_id, std::string* error) {
  std::string role;
  if (!getUserRole(user_id, group_id, &role, error)) {
    return false;
  }
  if (role == "owner") {
    if (error) {
      *error = "owner cannot leave group";
    }
    return false;
  }
  const std::string sql = "DELETE FROM group_members WHERE group_id = ? AND user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool GroupService::renameGroup(const std::string& actor_id,
                               const std::string& group_id,
                               const std::string& new_name,
                               std::string* error) {
  if (!isOwnerOrAdmin(actor_id, group_id, nullptr, error)) {
    return false;
  }
  const std::string sql = "UPDATE groups SET name = ? WHERE group_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, group_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool GroupService::kickUser(const std::string& actor_id,
                            const std::string& group_id,
                            const std::string& target_user_id,
                            std::string* error) {
  bool is_owner = false;
  if (!isOwnerOrAdmin(actor_id, group_id, &is_owner, error)) {
    return false;
  }
  std::string target_role;
  if (!getUserRole(target_user_id, group_id, &target_role, error)) {
    return false;
  }
  if (target_role == "owner") {
    if (error) {
      *error = "cannot kick owner";
    }
    return false;
  }
  if (!is_owner && target_role == "admin") {
    if (error) {
      *error = "admin cannot kick another admin";
    }
    return false;
  }
  const std::string sql = "DELETE FROM group_members WHERE group_id = ? AND user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, target_user_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool GroupService::dissolveGroup(const std::string& actor_id, const std::string& group_id, std::string* error) {
  bool is_owner = false;
  if (!isOwnerOrAdmin(actor_id, group_id, &is_owner, error)) {
    return false;
  }
  if (!is_owner) {
    if (error) {
      *error = "only owner can dissolve group";
    }
    return false;
  }

  if (!db_.execute("BEGIN;", error)) {
    return false;
  }

  bool ok = false;
  do {
    const std::string delete_targets =
        "DELETE FROM message_targets WHERE message_id IN "
        "(SELECT message_id FROM messages WHERE conversation_type = 'group' AND conversation_id = ?);";
    Statement target_stmt(db_.handle(), delete_targets, error);
    if (!target_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(target_stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(target_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string delete_messages =
        "DELETE FROM messages WHERE conversation_type = 'group' AND conversation_id = ?;";
    Statement message_stmt(db_.handle(), delete_messages, error);
    if (!message_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(message_stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(message_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string delete_members = "DELETE FROM group_members WHERE group_id = ?;";
    Statement member_stmt(db_.handle(), delete_members, error);
    if (!member_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(member_stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(member_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string delete_group = "DELETE FROM groups WHERE group_id = ?;";
    Statement group_stmt(db_.handle(), delete_group, error);
    if (!group_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(group_stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(group_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    ok = true;
  } while (false);

  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  return ok;
}

bool GroupService::setAdmin(const std::string& actor_id,
                            const std::string& group_id,
                            const std::string& target_user_id,
                            bool make_admin,
                            std::string* error) {
  bool is_owner = false;
  if (!isOwnerOrAdmin(actor_id, group_id, &is_owner, error)) {
    return false;
  }
  if (!is_owner) {
    if (error) {
      *error = "only owner can change admin role";
    }
    return false;
  }
  std::string target_role;
  if (!getUserRole(target_user_id, group_id, &target_role, error)) {
    return false;
  }
  if (target_role == "owner") {
    if (error) {
      *error = "cannot change owner role";
    }
    return false;
  }
  const std::string sql = "UPDATE group_members SET role = ? WHERE group_id = ? AND user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  const char* role = make_admin ? "admin" : "member";
  sqlite3_bind_text(stmt.get(), 1, role, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, target_user_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }
  return true;
}

bool GroupService::getGroupMembers(const std::string& group_id, std::vector<std::string>* members, std::string* error) {
  if (!members) {
    if (error) {
      *error = "members output is null";
    }
    return false;
  }
  members->clear();
  const std::string sql = "SELECT user_id FROM group_members WHERE group_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      const auto user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      if (user_id) {
        members->push_back(user_id);
      }
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

bool GroupService::getUserRole(const std::string& user_id,
                               const std::string& group_id,
                               std::string* role,
                               std::string* error) {
  if (!role) {
    if (error) {
      *error = "role output is null";
    }
    return false;
  }
  const std::string sql = "SELECT role FROM group_members WHERE group_id = ? AND user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    const auto value = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    if (value) {
      *role = value;
      return true;
    }
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "user not in group";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool GroupService::groupExists(const std::string& group_id, GroupInfo* info, std::string* error) {
  const std::string sql = "SELECT group_id, name, owner_id FROM groups WHERE group_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    if (info) {
      const auto gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      const auto name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
      const auto owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
      if (gid) info->group_id = gid;
      if (name) info->name = name;
      if (owner) info->owner_id = owner;
    }
    return true;
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "group not found";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool GroupService::isOwnerOrAdmin(const std::string& user_id,
                                  const std::string& group_id,
                                  bool* is_owner,
                                  std::string* error) {
  std::string role;
  if (!getUserRole(user_id, group_id, &role, error)) {
    return false;
  }
  if (is_owner) {
    *is_owner = (role == "owner");
  }
  if (role == "owner" || role == "admin") {
    return true;
  }
  if (error) {
    *error = "permission denied";
  }
  return false;
}

}  // namespace onlinetalk::server
