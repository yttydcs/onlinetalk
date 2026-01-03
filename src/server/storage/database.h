#pragma once

#include <sqlite3.h>

#include <string>

namespace onlinetalk::server {

class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql, std::string* error);
  ~Statement();
  sqlite3_stmt* get() const;
  bool valid() const;

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

class Database {
 public:
  Database() = default;
  ~Database();

  bool open(const std::string& path, std::string* error);
  void close();
  sqlite3* handle() const;

  bool initSchema(std::string* error);
  bool execute(const std::string& sql, std::string* error);

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace onlinetalk::server
