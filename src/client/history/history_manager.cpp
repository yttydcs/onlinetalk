#include "client/history/history_manager.h"

namespace onlinetalk::client {

void HistoryManager::reset(const std::string& key) {
  cursors_.erase(key);
}

void HistoryManager::update(const std::string& key, int64_t next_before_id, size_t count) {
  Cursor cursor;
  cursor.next_before_id = next_before_id;
  cursor.exhausted = (count == 0);
  cursors_[key] = cursor;
}

int64_t HistoryManager::nextBeforeId(const std::string& key) const {
  auto it = cursors_.find(key);
  if (it == cursors_.end()) {
    return 0;
  }
  return it->second.next_before_id;
}

bool HistoryManager::hasMore(const std::string& key) const {
  auto it = cursors_.find(key);
  if (it == cursors_.end()) {
    return true;
  }
  return !it->second.exhausted;
}

}  // namespace onlinetalk::client
