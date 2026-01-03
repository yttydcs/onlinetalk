#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/storage/database.h"

namespace onlinetalk::server {

struct FileOffer {
  std::string file_id;
  std::string conversation_type;
  std::string conversation_id;
  std::string file_name;
  int64_t file_size = 0;
  std::string sha256;
  std::string uploader_id;
  std::string uploader_nickname;
  std::vector<std::string> recipients;
};

struct UploadInfo {
  std::string file_id;
  std::string temp_path;
  std::string storage_path;
  std::string conversation_type;
  std::string conversation_id;
  std::string file_name;
  int64_t file_size = 0;
  int64_t uploaded_size = 0;
  std::string sha256;
  std::string uploader_id;
  std::string uploader_nickname;
  int64_t created_at = 0;
};

struct FileNotice {
  std::string file_id;
  std::string conversation_type;
  std::string conversation_id;
  std::string file_name;
  int64_t file_size = 0;
  std::string sha256;
  std::string uploader_id;
  std::string uploader_nickname;
  std::string storage_path;
  int64_t created_at = 0;
};

class FileService {
 public:
  FileService(Database& db, const std::string& data_dir, int chunk_size);

  bool ensureStorage(std::string* error);
  bool createUpload(const FileOffer& offer, UploadInfo* info, std::string* error);
  bool resumeUpload(const std::string& file_id, const std::string& uploader_id, UploadInfo* info, std::string* error);
  bool appendChunk(const std::string& file_id,
                   const std::string& uploader_id,
                   int64_t offset,
                   const std::vector<uint8_t>& data,
                   UploadInfo* info,
                   std::string* error);
  bool finalizeUpload(const std::string& file_id,
                      const std::string& uploader_id,
                      FileNotice* notice,
                      std::string* error);

  bool fetchUndelivered(const std::string& user_id,
                        int limit,
                        std::vector<FileNotice>* out,
                        std::string* error);
  bool markDelivered(const std::string& user_id,
                     const std::vector<std::string>& file_ids,
                     std::string* error);

  bool readChunk(const std::string& file_id,
                 const std::string& user_id,
                 int64_t offset,
                 std::vector<uint8_t>* data,
                 FileNotice* notice,
                 std::string* error);
  bool listTargets(const std::string& file_id,
                   std::vector<std::string>* targets,
                   std::string* error);

  int chunkSize() const;

 private:
  bool getUploadInfo(const std::string& file_id, UploadInfo* info, std::string* error);
  bool getFileNotice(const std::string& file_id, FileNotice* notice, std::string* error);
  bool hasDownloadPermission(const std::string& file_id, const std::string& user_id, std::string* error);
  bool isUploading(const std::string& file_id, std::string* error);

  std::string data_dir_;
  std::string files_dir_;
  std::string temp_dir_;
  int chunk_size_ = 0;
  Database& db_;
};

}  // namespace onlinetalk::server
