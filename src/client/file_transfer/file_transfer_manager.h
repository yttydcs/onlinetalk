#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "client/file_transfer/transfer_state.h"
#include "client/net/net_client.h"

namespace onlinetalk::client {

struct UploadRequest {
  std::string conversation_type;
  std::string conversation_id;
  std::string file_path;
  std::string file_id;
};

struct DownloadRequest {
  std::string conversation_type;
  std::string conversation_id;
  std::string file_id;
  std::string file_name;
  int64_t file_size = 0;
  std::string sha256;
};

class FileTransferManager {
 public:
  explicit FileTransferManager(const std::string& data_dir);

  bool beginUpload(NetClient& net,
                   const UploadRequest& request,
                   uint64_t* request_id,
                   std::string* error);
  bool beginDownload(NetClient& net,
                     const DownloadRequest& request,
                     uint64_t* request_id,
                     std::string* error);

  bool handlePacket(NetClient& net, const onlinetalk::common::Packet& packet);
  bool resumeTransfers(NetClient& net, std::string* error);

  const std::unordered_map<std::string, TransferState>& uploadStates() const;
  const std::unordered_map<std::string, TransferState>& downloadStates() const;
  const std::string& lastError() const;

 private:
  struct UploadTask {
    uint64_t request_id = 0;
    std::string file_id;
    std::string conversation_type;
    std::string conversation_id;
    std::string file_path;
    std::string file_name;
    std::string sha256;
    int64_t file_size = 0;
    int64_t next_offset = 0;
    int chunk_size = 0;
    std::shared_ptr<std::ifstream> stream;
    bool done = false;
    bool failed = false;
  };

  struct DownloadTask {
    std::string file_id;
    std::string conversation_type;
    std::string conversation_id;
    std::string file_name;
    std::string sha256;
    int64_t file_size = 0;
    int64_t next_offset = 0;
    std::string temp_path;
    std::string final_path;
    bool done = false;
    bool failed = false;
  };

  bool handleFileAccept(NetClient& net, uint64_t request_id, const nlohmann::json& meta);
  bool handleUploadAck(NetClient& net, uint64_t request_id, const nlohmann::json& meta);
  bool handleDownloadChunk(NetClient& net, const onlinetalk::common::Packet& packet, const nlohmann::json& meta);

  void markUploadFailed(const std::string& file_id, const std::string& message);
  void markDownloadFailed(const std::string& file_id, const std::string& message);
  void eraseUploadMapping(const std::string& file_id);

  bool sendNextChunk(NetClient& net, UploadTask& task, std::string* error);
  bool sendUploadDone(NetClient& net, const UploadTask& task, std::string* error);
  bool sendDownloadRequest(NetClient& net, const DownloadTask& task, uint64_t request_id, std::string* error);

  static std::string sanitizeFileName(const std::string& name);
  std::string downloadDir(const std::string& conversation_type, const std::string& conversation_id) const;

  std::string data_dir_;
  std::unordered_map<uint64_t, UploadTask> pending_offers_;
  std::unordered_map<uint64_t, std::string> upload_request_map_;
  std::unordered_map<uint64_t, std::string> download_request_map_;
  std::unordered_map<std::string, UploadTask> uploads_;
  std::unordered_map<std::string, DownloadTask> downloads_;
  std::unordered_map<std::string, TransferState> upload_states_;
  std::unordered_map<std::string, TransferState> download_states_;
  std::string last_error_;
};

}  // namespace onlinetalk::client
