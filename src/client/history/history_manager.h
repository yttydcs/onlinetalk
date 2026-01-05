#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace onlinetalk::client {

class HistoryManager {
 public:
  void reset(const std::string& key);
  void update(const std::string& key, int64_t next_before_id, size_t count);
  int64_t nextBeforeId(const std::string& key) const;
  bool hasMore(const std::string& key) const;

 private:
  struct Cursor {
    int64_t next_before_id = 0;
    bool exhausted = false;
  };

  std::unordered_map<std::string, Cursor> cursors_;
};

}  // namespace onlinetalk::client
