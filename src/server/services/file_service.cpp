#include "server/services/file_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "common/crypto/sha256.h"
#include "common/fs.h"
#include "server/services/id_generator.h"

namespace onlinetalk::server {

namespace {

int64_t nowSeconds() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string sanitizeFileName(const std::string& name) {
  std::string sanitized;
  sanitized.reserve(name.size());
  for (const char ch : name) {
    if ((ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '.' || ch == '_' || ch == '-') {
      sanitized.push_back(ch);
    } else {
      sanitized.push_back('_');
    }
  }
  if (sanitized.empty()) {
    return "file";
  }
  return sanitized;
}

bool ensureParentDir(const std::string& path, std::string* error) {
  const auto dir = std::filesystem::path(path).parent_path();
  if (dir.empty()) {
    return true;
  }
  return onlinetalk::common::ensureDirectory(dir.string(), error);
}

}  // namespace

FileService::FileService(Database& db, const std::string& data_dir, int chunk_size)
    : data_dir_(data_dir),
      files_dir_(data_dir + "/files"),
      temp_dir_(data_dir + "/tmp"),
      chunk_size_(chunk_size),
      db_(db) {}

bool FileService::ensureStorage(std::string* error) {
  return onlinetalk::common::ensureDirectory(files_dir_, error) &&
         onlinetalk::common::ensureDirectory(temp_dir_, error);
}

bool FileService::createUpload(const FileOffer& offer, UploadInfo* info, std::string* error) {
  if (!info) {
    if (error) {
      *error = "upload info is null";
    }
    return false;
  }
  if (offer.file_size <= 0) {
    if (error) {
      *error = "file_size must be positive";
    }
    return false;
  }
  if (offer.recipients.empty()) {
    if (error) {
      *error = "recipients empty";
    }
    return false;
  }

  const std::string file_id = generateId();
  const std::string safe_name = sanitizeFileName(offer.file_name);
  const std::string storage_path = files_dir_ + "/" + file_id + "_" + safe_name;
  const std::string temp_path = temp_dir_ + "/" + file_id + ".part";

  if (!ensureParentDir(storage_path, error) || !ensureParentDir(temp_path, error)) {
    return false;
  }

  std::unordered_set<std::string> unique_targets(offer.recipients.begin(), offer.recipients.end());
  const auto created_at = nowSeconds();

  if (!db_.execute("BEGIN;", error)) {
    return false;
  }
  bool ok = false;
  do {
    const std::string insert_file =
        "INSERT INTO files(file_id, uploader_id, uploader_nickname, conversation_type, conversation_id, "
        "file_name, file_size, sha256, storage_path, created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?);";
    Statement file_stmt(db_.handle(), insert_file, error);
    if (!file_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(file_stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 2, offer.uploader_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 3, offer.uploader_nickname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 4, offer.conversation_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 5, offer.conversation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 6, offer.file_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(file_stmt.get(), 7, offer.file_size);
    sqlite3_bind_text(file_stmt.get(), 8, offer.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(file_stmt.get(), 9, storage_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(file_stmt.get(), 10, created_at);
    if (sqlite3_step(file_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string insert_upload =
        "INSERT INTO file_uploads(file_id, uploader_id, temp_path, uploaded_size, status, updated_at) "
        "VALUES(?,?,?,?,?,?);";
    Statement upload_stmt(db_.handle(), insert_upload, error);
    if (!upload_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(upload_stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upload_stmt.get(), 2, offer.uploader_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upload_stmt.get(), 3, temp_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upload_stmt.get(), 4, 0);
    sqlite3_bind_text(upload_stmt.get(), 5, "uploading", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upload_stmt.get(), 6, created_at);
    if (sqlite3_step(upload_stmt.get()) != SQLITE_DONE) {
      if (error) {
        *error = sqlite3_errmsg(db_.handle());
      }
      break;
    }

    const std::string insert_target =
        "INSERT INTO file_targets(file_id, user_id, delivered_at) VALUES(?,?,NULL);";
    Statement target_stmt(db_.handle(), insert_target, error);
    if (!target_stmt.valid()) {
      break;
    }
    for (const auto& user_id : unique_targets) {
      sqlite3_reset(target_stmt.get());
      sqlite3_clear_bindings(target_stmt.get());
      sqlite3_bind_text(target_stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(target_stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(target_stmt.get()) != SQLITE_DONE) {
        if (error) {
          *error = sqlite3_errmsg(db_.handle());
        }
        break;
      }
    }
    if (error && !error->empty()) {
      break;
    }
    ok = true;
  } while (false);

  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  if (!ok) {
    return false;
  }

  info->file_id = file_id;
  info->temp_path = temp_path;
  info->storage_path = storage_path;
  info->conversation_type = offer.conversation_type;
  info->conversation_id = offer.conversation_id;
  info->file_name = offer.file_name;
  info->file_size = offer.file_size;
  info->uploaded_size = 0;
  info->sha256 = offer.sha256;
  info->uploader_id = offer.uploader_id;
  info->uploader_nickname = offer.uploader_nickname;
  info->created_at = created_at;
  return true;
}

bool FileService::resumeUpload(const std::string& file_id,
                               const std::string& uploader_id,
                               UploadInfo* info,
                               std::string* error) {
  if (!info) {
    if (error) {
      *error = "upload info is null";
    }
    return false;
  }
  UploadInfo current;
  if (!getUploadInfo(file_id, &current, error)) {
    return false;
  }
  if (current.uploader_id != uploader_id) {
    if (error) {
      *error = "uploader mismatch";
    }
    return false;
  }

  std::error_code ec;
  const auto actual_size =
      std::filesystem::exists(current.temp_path, ec) ? std::filesystem::file_size(current.temp_path, ec) : 0;
  if (!ec && static_cast<int64_t>(actual_size) != current.uploaded_size) {
    const std::string sql = "UPDATE file_uploads SET uploaded_size = ?, updated_at = ? WHERE file_id = ?;";
    std::string update_error;
    Statement stmt(db_.handle(), sql, &update_error);
    if (stmt.valid()) {
      sqlite3_bind_int64(stmt.get(), 1, static_cast<int64_t>(actual_size));
      sqlite3_bind_int64(stmt.get(), 2, nowSeconds());
      sqlite3_bind_text(stmt.get(), 3, file_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt.get());
    }
    current.uploaded_size = static_cast<int64_t>(actual_size);
  }

  *info = current;
  return true;
}

bool FileService::appendChunk(const std::string& file_id,
                              const std::string& uploader_id,
                              int64_t offset,
                              const std::vector<uint8_t>& data,
                              UploadInfo* info,
                              std::string* error) {
  if (!info) {
    if (error) {
      *error = "upload info is null";
    }
    return false;
  }
  UploadInfo current;
  if (!getUploadInfo(file_id, &current, error)) {
    return false;
  }
  if (current.uploader_id != uploader_id) {
    if (error) {
      *error = "uploader mismatch";
    }
    return false;
  }
  if (offset != current.uploaded_size) {
    if (error) {
      *error = "offset mismatch";
    }
    return false;
  }
  if (offset + static_cast<int64_t>(data.size()) > current.file_size) {
    if (error) {
      *error = "chunk exceeds file size";
    }
    return false;
  }

  std::fstream stream;
  if (offset == 0) {
    stream.open(current.temp_path, std::ios::binary | std::ios::out | std::ios::trunc);
  } else {
    stream.open(current.temp_path, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!stream.is_open()) {
    if (error) {
      *error = "failed to open temp file";
    }
    return false;
  }
  stream.seekp(offset, std::ios::beg);
  stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!stream.good()) {
    if (error) {
      *error = "failed to write temp file";
    }
    return false;
  }
  stream.flush();

  const int64_t next_offset = offset + static_cast<int64_t>(data.size());
  const std::string sql = "UPDATE file_uploads SET uploaded_size = ?, updated_at = ? WHERE file_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_int64(stmt.get(), 1, next_offset);
  sqlite3_bind_int64(stmt.get(), 2, nowSeconds());
  sqlite3_bind_text(stmt.get(), 3, file_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    if (error) {
      *error = sqlite3_errmsg(db_.handle());
    }
    return false;
  }

  current.uploaded_size = next_offset;
  *info = current;
  return true;
}

bool FileService::finalizeUpload(const std::string& file_id,
                                 const std::string& uploader_id,
                                 FileNotice* notice,
                                 std::string* error) {
  if (!notice) {
    if (error) {
      *error = "notice is null";
    }
    return false;
  }
  UploadInfo current;
  if (!getUploadInfo(file_id, &current, error)) {
    return false;
  }
  if (current.uploader_id != uploader_id) {
    if (error) {
      *error = "uploader mismatch";
    }
    return false;
  }
  if (current.uploaded_size != current.file_size) {
    if (error) {
      *error = "file not fully uploaded";
    }
    return false;
  }

  std::string hash_error;
  const auto computed = onlinetalk::common::sha256HexFile(current.temp_path, &hash_error);
  if (!hash_error.empty()) {
    if (error) {
      *error = hash_error;
    }
    return false;
  }
  if (computed != current.sha256) {
    if (error) {
      *error = "sha256 mismatch";
    }
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(current.temp_path, current.storage_path, ec);
  if (ec) {
    if (error) {
      *error = "failed to move file to storage path";
    }
    return false;
  }

  if (!db_.execute("BEGIN;", error)) {
    return false;
  }
  bool ok = false;
  do {
    const std::string delete_upload = "DELETE FROM file_uploads WHERE file_id = ?;";
    Statement del_stmt(db_.handle(), delete_upload, error);
    if (!del_stmt.valid()) {
      break;
    }
    sqlite3_bind_text(del_stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(del_stmt.get()) != SQLITE_DONE) {
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
  if (!ok) {
    return false;
  }

  FileNotice record;
  if (!getFileNotice(file_id, &record, error)) {
    return false;
  }
  *notice = record;
  return true;
}

bool FileService::fetchUndelivered(const std::string& user_id,
                                   int limit,
                                   std::vector<FileNotice>* out,
                                   std::string* error) {
  if (!out) {
    if (error) {
      *error = "output list is null";
    }
    return false;
  }
  out->clear();
  const std::string sql =
      "SELECT f.file_id, f.conversation_type, f.conversation_id, f.file_name, f.file_size, "
      "f.sha256, f.uploader_id, f.uploader_nickname, f.storage_path, f.created_at "
      "FROM file_targets t "
      "JOIN files f ON t.file_id = f.file_id "
      "LEFT JOIN file_uploads u ON f.file_id = u.file_id "
      "WHERE t.user_id = ? AND t.delivered_at IS NULL AND u.file_id IS NULL "
      "ORDER BY f.created_at ASC LIMIT ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 2, limit);
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      FileNotice notice;
      notice.file_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      notice.conversation_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
      notice.conversation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
      notice.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
      notice.file_size = sqlite3_column_int64(stmt.get(), 4);
      notice.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
      notice.uploader_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
      notice.uploader_nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
      notice.storage_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
      notice.created_at = sqlite3_column_int64(stmt.get(), 9);
      out->push_back(std::move(notice));
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

bool FileService::markDelivered(const std::string& user_id,
                                const std::vector<std::string>& file_ids,
                                std::string* error) {
  if (file_ids.empty()) {
    return true;
  }
  if (!db_.execute("BEGIN;", error)) {
    return false;
  }
  bool ok = false;
  do {
    const std::string sql =
        "UPDATE file_targets SET delivered_at = ? WHERE user_id = ? AND file_id = ?;";
    Statement stmt(db_.handle(), sql, error);
    if (!stmt.valid()) {
      break;
    }
    const auto delivered_at = nowSeconds();
    for (const auto& file_id : file_ids) {
      sqlite3_reset(stmt.get());
      sqlite3_clear_bindings(stmt.get());
      sqlite3_bind_int64(stmt.get(), 1, delivered_at);
      sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.get(), 3, file_id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        if (error) {
          *error = sqlite3_errmsg(db_.handle());
        }
        break;
      }
    }
    if (error && !error->empty()) {
      break;
    }
    ok = true;
  } while (false);
  if (!db_.execute(ok ? "COMMIT;" : "ROLLBACK;", error)) {
    return false;
  }
  return ok;
}

bool FileService::readChunk(const std::string& file_id,
                            const std::string& user_id,
                            int64_t offset,
                            std::vector<uint8_t>* data,
                            FileNotice* notice,
                            std::string* error) {
  if (!data || !notice) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (!hasDownloadPermission(file_id, user_id, error)) {
    return false;
  }
  const bool uploading = isUploading(file_id, error);
  if (error && !error->empty()) {
    return false;
  }
  if (uploading) {
    if (error) {
      *error = "file is still uploading";
    }
    return false;
  }
  FileNotice record;
  if (!getFileNotice(file_id, &record, error)) {
    return false;
  }
  if (offset < 0 || offset >= record.file_size) {
    if (error) {
      *error = "offset out of range";
    }
    return false;
  }

  std::ifstream file(record.storage_path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "failed to open file";
    }
    return false;
  }
  file.seekg(offset, std::ios::beg);
  const int64_t remaining = record.file_size - offset;
  const int64_t to_read = std::min<int64_t>(remaining, chunk_size_);
  data->assign(static_cast<size_t>(to_read), 0);
  file.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(to_read));
  if (!file.good() && !file.eof()) {
    if (error) {
      *error = "failed to read file";
    }
    return false;
  }
  data->resize(static_cast<size_t>(file.gcount()));
  *notice = record;
  return true;
}

bool FileService::listTargets(const std::string& file_id, std::vector<std::string>* targets, std::string* error) {
  if (!targets) {
    if (error) {
      *error = "targets output is null";
    }
    return false;
  }
  targets->clear();
  const std::string sql = "SELECT user_id FROM file_targets WHERE file_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      const auto user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      if (user_id) {
        targets->push_back(user_id);
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

int FileService::chunkSize() const {
  return chunk_size_;
}

bool FileService::getUploadInfo(const std::string& file_id, UploadInfo* info, std::string* error) {
  if (!info) {
    if (error) {
      *error = "upload info is null";
    }
    return false;
  }
  const std::string sql =
      "SELECT f.file_id, f.conversation_type, f.conversation_id, f.file_name, f.file_size, f.sha256, "
      "f.uploader_id, f.uploader_nickname, f.storage_path, f.created_at, "
      "u.temp_path, u.uploaded_size "
      "FROM files f JOIN file_uploads u ON f.file_id = u.file_id WHERE f.file_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    info->file_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    info->conversation_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    info->conversation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    info->file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    info->file_size = sqlite3_column_int64(stmt.get(), 4);
    info->sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
    info->uploader_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    info->uploader_nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    info->storage_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
    info->created_at = sqlite3_column_int64(stmt.get(), 9);
    info->temp_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 10));
    info->uploaded_size = sqlite3_column_int64(stmt.get(), 11);
    return true;
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "upload not found";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool FileService::getFileNotice(const std::string& file_id, FileNotice* notice, std::string* error) {
  if (!notice) {
    if (error) {
      *error = "notice is null";
    }
    return false;
  }
  const std::string sql =
      "SELECT file_id, conversation_type, conversation_id, file_name, file_size, sha256, "
      "uploader_id, uploader_nickname, storage_path, created_at "
      "FROM files WHERE file_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    notice->file_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    notice->conversation_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    notice->conversation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    notice->file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    notice->file_size = sqlite3_column_int64(stmt.get(), 4);
    notice->sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
    notice->uploader_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    notice->uploader_nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    notice->storage_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
    notice->created_at = sqlite3_column_int64(stmt.get(), 9);
    return true;
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "file not found";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool FileService::hasDownloadPermission(const std::string& file_id,
                                        const std::string& user_id,
                                        std::string* error) {
  const std::string sql = "SELECT 1 FROM file_targets WHERE file_id = ? AND user_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    if (error) {
      *error = "no permission to download";
    }
    return false;
  }
  if (error) {
    *error = sqlite3_errmsg(db_.handle());
  }
  return false;
}

bool FileService::isUploading(const std::string& file_id, std::string* error) {
  const std::string sql = "SELECT 1 FROM file_uploads WHERE file_id = ?;";
  Statement stmt(db_.handle(), sql, error);
  if (!stmt.valid()) {
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, file_id.c_str(), -1, SQLITE_TRANSIENT);
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

}  // namespace onlinetalk::server
