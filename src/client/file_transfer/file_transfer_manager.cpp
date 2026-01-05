#include "client/file_transfer/file_transfer_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "common/crypto/sha256.h"
#include "common/fs.h"

namespace onlinetalk::client {

namespace {

bool parseJson(const std::string& text, nlohmann::json* out, std::string* error) {
  try {
    *out = nlohmann::json::parse(text);
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("invalid json: ") + ex.what();
    }
    return false;
  }
}

bool sendFileOffer(NetClient& net,
                   const std::string& conversation_type,
                   const std::string& conversation_id,
                   const std::string& file_name,
                   int64_t file_size,
                   const std::string& sha256,
                   const std::string& file_id,
                   uint64_t request_id,
                   std::string* error) {
  nlohmann::json meta;
  meta["conversation_type"] = conversation_type;
  meta["conversation_id"] = conversation_id;
  meta["file_name"] = file_name;
  meta["file_size"] = file_size;
  meta["sha256"] = sha256;
  if (!file_id.empty()) {
    meta["file_id"] = file_id;
  }
  if (!net.sendJson(onlinetalk::common::PacketType::FileOffer, request_id, meta, nullptr)) {
    if (error) {
      *error = "failed to send file offer";
    }
    return false;
  }
  return true;
}

}  // namespace

FileTransferManager::FileTransferManager(const std::string& data_dir) : data_dir_(data_dir) {}

bool FileTransferManager::beginUpload(NetClient& net,
                                      const UploadRequest& request,
                                      uint64_t* request_id,
                                      std::string* error) {
  if (request.conversation_type.empty() || request.conversation_id.empty()) {
    if (error) {
      *error = "conversation info required";
    }
    return false;
  }
  if (request.file_path.empty()) {
    if (error) {
      *error = "file_path required";
    }
    return false;
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(request.file_path, ec);
  if (ec) {
    if (error) {
      *error = "failed to stat file";
    }
    return false;
  }
  if (size == 0) {
    if (error) {
      *error = "file is empty";
    }
    return false;
  }

  std::string hash_error;
  const auto sha256 = onlinetalk::common::sha256HexFile(request.file_path, &hash_error);
  if (!hash_error.empty()) {
    if (error) {
      *error = hash_error;
    }
    return false;
  }

  const auto file_name = std::filesystem::path(request.file_path).filename().string();
  const auto req_id = net.nextRequestId();
  if (!sendFileOffer(net,
                     request.conversation_type,
                     request.conversation_id,
                     file_name,
                     static_cast<int64_t>(size),
                     sha256,
                     request.file_id,
                     req_id,
                     error)) {
    return false;
  }

  UploadTask task;
  task.request_id = req_id;
  task.file_id = request.file_id;
  task.conversation_type = request.conversation_type;
  task.conversation_id = request.conversation_id;
  task.file_path = request.file_path;
  task.file_name = file_name;
  task.sha256 = sha256;
  task.file_size = static_cast<int64_t>(size);
  pending_offers_[req_id] = task;
  if (request_id) {
    *request_id = req_id;
  }
  return true;
}

bool FileTransferManager::resumeTransfers(NetClient& net, std::string* error) {
  download_request_map_.clear();

  std::vector<UploadTask> pending;
  pending.reserve(pending_offers_.size());
  for (const auto& entry : pending_offers_) {
    pending.push_back(entry.second);
  }
  pending_offers_.clear();

  for (auto& task : pending) {
    if (task.failed || task.done) {
      continue;
    }
    const auto req_id = net.nextRequestId();
    if (!sendFileOffer(net,
                       task.conversation_type,
                       task.conversation_id,
                       task.file_name,
                       task.file_size,
                       task.sha256,
                       task.file_id,
                       req_id,
                       error)) {
      return false;
    }
    task.request_id = req_id;
    task.stream.reset();
    pending_offers_[req_id] = task;
  }

  for (auto& entry : uploads_) {
    auto& task = entry.second;
    if (task.failed || task.done) {
      continue;
    }
    eraseUploadMapping(task.file_id);
    const auto req_id = net.nextRequestId();
    if (!sendFileOffer(net,
                       task.conversation_type,
                       task.conversation_id,
                       task.file_name,
                       task.file_size,
                       task.sha256,
                       task.file_id,
                       req_id,
                       error)) {
      return false;
    }
    task.request_id = req_id;
    task.stream.reset();
    pending_offers_[req_id] = task;
  }

  for (auto& entry : downloads_) {
    auto& task = entry.second;
    if (task.failed || task.done) {
      continue;
    }
    const auto req_id = net.nextRequestId();
    if (!sendDownloadRequest(net, task, req_id, error)) {
      return false;
    }
  }
  return true;
}

bool FileTransferManager::beginDownload(NetClient& net,
                                        const DownloadRequest& request,
                                        uint64_t* request_id,
                                        std::string* error) {
  if (request.file_id.empty()) {
    if (error) {
      *error = "file_id required";
    }
    return false;
  }
  if (request.file_size <= 0) {
    if (error) {
      *error = "invalid file_size";
    }
    return false;
  }
  if (request.sha256.empty()) {
    if (error) {
      *error = "sha256 required";
    }
    return false;
  }

  const auto dir = downloadDir(request.conversation_type, request.conversation_id);
  if (!onlinetalk::common::ensureDirectory(dir, error)) {
    return false;
  }

  const auto safe_name = sanitizeFileName(request.file_name);
  const auto base_path = dir + "/" + request.file_id + "_" + safe_name;
  const auto temp_path = base_path + ".part";

  int64_t offset = 0;
  std::error_code ec;
  if (std::filesystem::exists(temp_path, ec)) {
    const auto existing = std::filesystem::file_size(temp_path, ec);
    if (!ec && existing > 0) {
      offset = static_cast<int64_t>(existing);
      if (offset >= request.file_size) {
        offset = 0;
      }
    }
  }
  if (offset == 0) {
    std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      if (error) {
        *error = "failed to create temp file";
      }
      return false;
    }
  }

  DownloadTask task;
  task.file_id = request.file_id;
  task.conversation_type = request.conversation_type;
  task.conversation_id = request.conversation_id;
  task.file_name = request.file_name;
  task.sha256 = request.sha256;
  task.file_size = request.file_size;
  task.next_offset = offset;
  task.temp_path = temp_path;
  task.final_path = base_path;
  downloads_[task.file_id] = task;

  TransferState state;
  state.file_id = task.file_id;
  state.file_name = task.file_name;
  state.total_size = task.file_size;
  state.transferred = task.next_offset;
  download_states_[task.file_id] = state;

  const auto req_id = net.nextRequestId();
  if (!sendDownloadRequest(net, downloads_[task.file_id], req_id, error)) {
    return false;
  }
  if (request_id) {
    *request_id = req_id;
  }
  return true;
}

bool FileTransferManager::handlePacket(NetClient& net, const onlinetalk::common::Packet& packet) {
  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  if (type != onlinetalk::common::PacketType::FileOffer &&
      type != onlinetalk::common::PacketType::FileAccept &&
      type != onlinetalk::common::PacketType::FileUploadChunk &&
      type != onlinetalk::common::PacketType::FileUploadDone &&
      type != onlinetalk::common::PacketType::FileDownloadRequest &&
      type != onlinetalk::common::PacketType::FileDownloadChunk &&
      type != onlinetalk::common::PacketType::FileDone) {
    return false;
  }

  nlohmann::json meta;
  std::string error;
  if (!parseJson(packet.meta_json, &meta, &error)) {
    last_error_ = error;
    return true;
  }

  const auto status = meta.value("status", "");
  if (!status.empty() && status != "ok") {
    const auto message = meta.value("message", "request failed");
    if (type == onlinetalk::common::PacketType::FileOffer) {
      auto it = pending_offers_.find(packet.header.request_id);
      if (it != pending_offers_.end()) {
        if (!it->second.file_id.empty()) {
          markUploadFailed(it->second.file_id, message);
        } else {
          last_error_ = message;
        }
        pending_offers_.erase(it);
      } else {
        last_error_ = message;
      }
      return true;
    }
    if (type == onlinetalk::common::PacketType::FileUploadDone) {
      auto it = upload_request_map_.find(packet.header.request_id);
      if (it != upload_request_map_.end()) {
        markUploadFailed(it->second, message);
        eraseUploadMapping(it->second);
      } else {
        last_error_ = message;
      }
      return true;
    }
    if (type == onlinetalk::common::PacketType::FileDownloadRequest) {
      auto it = download_request_map_.find(packet.header.request_id);
      if (it != download_request_map_.end()) {
        markDownloadFailed(it->second, message);
        download_request_map_.erase(it);
      } else {
        last_error_ = message;
      }
      return true;
    }
  }

  switch (type) {
    case onlinetalk::common::PacketType::FileAccept:
      return handleFileAccept(net, packet.header.request_id, meta);
    case onlinetalk::common::PacketType::FileUploadChunk:
      return handleUploadAck(net, packet.header.request_id, meta);
    case onlinetalk::common::PacketType::FileDownloadChunk:
      return handleDownloadChunk(net, packet, meta);
    case onlinetalk::common::PacketType::FileDone: {
      const auto file_id = meta.value("file_id", "");
      auto it = uploads_.find(file_id);
      if (it != uploads_.end()) {
        it->second.done = true;
        it->second.stream.reset();
        auto state_it = upload_states_.find(file_id);
        if (state_it != upload_states_.end()) {
          state_it->second.done = true;
          state_it->second.transferred = it->second.file_size;
        }
        eraseUploadMapping(file_id);
      }
      return true;
    }
    default:
      return false;
  }
}

const std::unordered_map<std::string, TransferState>& FileTransferManager::uploadStates() const {
  return upload_states_;
}

const std::unordered_map<std::string, TransferState>& FileTransferManager::downloadStates() const {
  return download_states_;
}

const std::string& FileTransferManager::lastError() const {
  return last_error_;
}

bool FileTransferManager::handleFileAccept(NetClient& net, uint64_t request_id, const nlohmann::json& meta) {
  const auto status = meta.value("status", "");
  if (!status.empty() && status != "ok") {
    auto it = pending_offers_.find(request_id);
    if (it != pending_offers_.end()) {
      pending_offers_.erase(it);
    }
    last_error_ = meta.value("message", "file accept failed");
    return true;
  }

  auto it = pending_offers_.find(request_id);
  if (it == pending_offers_.end()) {
    return true;
  }
  UploadTask task = it->second;
  pending_offers_.erase(it);

  task.file_id = meta.value("file_id", task.file_id);
  task.next_offset = meta.value("next_offset", static_cast<int64_t>(0));
  task.chunk_size = meta.value("chunk_size", 0);
  if (task.file_id.empty() || task.chunk_size <= 0) {
    last_error_ = "invalid file accept response";
    return true;
  }
  uploads_[task.file_id] = task;
  upload_request_map_[task.request_id] = task.file_id;

  TransferState state;
  state.file_id = task.file_id;
  state.file_name = task.file_name;
  state.total_size = task.file_size;
  state.transferred = task.next_offset;
  upload_states_[task.file_id] = state;

  std::string error;
  if (!sendNextChunk(net, uploads_[task.file_id], &error)) {
    last_error_ = error;
  }
  return true;
}

bool FileTransferManager::handleUploadAck(NetClient& net, uint64_t request_id, const nlohmann::json& meta) {
  const auto status = meta.value("status", "");
  auto file_it = upload_request_map_.find(request_id);
  if (file_it == upload_request_map_.end()) {
    return true;
  }
  auto upload_it = uploads_.find(file_it->second);
  if (upload_it == uploads_.end()) {
    return true;
  }

  UploadTask& task = upload_it->second;
  if (!status.empty() && status != "ok") {
    task.failed = true;
    auto state_it = upload_states_.find(task.file_id);
    if (state_it != upload_states_.end()) {
      state_it->second.failed = true;
    }
    last_error_ = meta.value("message", "upload failed");
    const auto expected = meta.value("expected_offset", static_cast<int64_t>(task.next_offset));
    task.next_offset = expected;
    upload_request_map_.erase(request_id);
    task.stream.reset();
    return true;
  }

  task.next_offset = meta.value("next_offset", task.next_offset);
  auto state_it = upload_states_.find(task.file_id);
  if (state_it != upload_states_.end()) {
    state_it->second.transferred = task.next_offset;
  }

  std::string error;
  if (task.next_offset >= task.file_size) {
    if (!sendUploadDone(net, task, &error)) {
      last_error_ = error;
    }
    return true;
  }
  if (!sendNextChunk(net, task, &error)) {
    last_error_ = error;
  }
  return true;
}

bool FileTransferManager::handleDownloadChunk(NetClient& net,
                                              const onlinetalk::common::Packet& packet,
                                              const nlohmann::json& meta) {
  download_request_map_.erase(packet.header.request_id);
  const auto file_id = meta.value("file_id", "");
  auto it = downloads_.find(file_id);
  if (it == downloads_.end()) {
    return true;
  }
  DownloadTask& task = it->second;

  const auto offset = meta.value("offset", task.next_offset);
  if (offset != task.next_offset) {
    markDownloadFailed(task.file_id, "download offset mismatch");
    return true;
  }
  if (packet.binary.empty() && !meta.value("done", false)) {
    markDownloadFailed(task.file_id, "download chunk empty");
    return true;
  }

  std::fstream stream;
  if (offset == 0) {
    stream.open(task.temp_path, std::ios::binary | std::ios::out | std::ios::trunc);
  } else {
    stream.open(task.temp_path, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!stream.is_open()) {
    markDownloadFailed(task.file_id, "failed to open temp file");
    return true;
  }
  stream.seekp(offset, std::ios::beg);
  if (!packet.binary.empty()) {
    stream.write(reinterpret_cast<const char*>(packet.binary.data()),
                 static_cast<std::streamsize>(packet.binary.size()));
  }
  stream.flush();
  const int64_t next_offset = offset + static_cast<int64_t>(packet.binary.size());
  task.next_offset = next_offset;

  auto state_it = download_states_.find(task.file_id);
  if (state_it != download_states_.end()) {
    state_it->second.transferred = next_offset;
  }

  const bool done = meta.value("done", false);
  if (done || next_offset >= task.file_size) {
    std::string hash_error;
    const auto computed = onlinetalk::common::sha256HexFile(task.temp_path, &hash_error);
    if (!hash_error.empty() || computed != task.sha256) {
      markDownloadFailed(task.file_id, hash_error.empty() ? "sha256 mismatch" : hash_error);
      return true;
    }
    std::error_code ec;
    std::filesystem::rename(task.temp_path, task.final_path, ec);
    if (ec) {
      markDownloadFailed(task.file_id, "failed to move download");
      return true;
    }
    task.done = true;
    if (state_it != download_states_.end()) {
      state_it->second.done = true;
      state_it->second.transferred = task.file_size;
    }
    return true;
  }

  std::string error;
  const auto req_id = net.nextRequestId();
  if (!sendDownloadRequest(net, task, req_id, &error)) {
    last_error_ = error;
  }
  return true;
}

bool FileTransferManager::sendNextChunk(NetClient& net, UploadTask& task, std::string* error) {
  if (task.chunk_size <= 0) {
    if (error) {
      *error = "invalid chunk size";
    }
    return false;
  }
  if (task.next_offset >= task.file_size) {
    return sendUploadDone(net, task, error);
  }

  const int64_t remaining = task.file_size - task.next_offset;
  const int64_t to_read = std::min<int64_t>(remaining, task.chunk_size);
  std::vector<uint8_t> data(static_cast<size_t>(to_read));

  if (!task.stream) {
    task.stream = std::make_shared<std::ifstream>(task.file_path, std::ios::binary);
  }
  if (!task.stream || !task.stream->is_open()) {
    if (error) {
      *error = "failed to open upload file";
    }
    return false;
  }
  task.stream->clear();
  task.stream->seekg(task.next_offset, std::ios::beg);
  task.stream->read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(to_read));
  const auto read_size = task.stream->gcount();
  if (read_size <= 0) {
    if (error) {
      *error = "failed to read upload file";
    }
    return false;
  }
  data.resize(static_cast<size_t>(read_size));

  nlohmann::json meta;
  meta["file_id"] = task.file_id;
  meta["offset"] = task.next_offset;
  if (!net.sendJson(onlinetalk::common::PacketType::FileUploadChunk, task.request_id, meta, &data)) {
    if (error) {
      *error = "failed to send upload chunk";
    }
    return false;
  }
  return true;
}

bool FileTransferManager::sendUploadDone(NetClient& net, const UploadTask& task, std::string* error) {
  nlohmann::json meta;
  meta["file_id"] = task.file_id;
  if (!net.sendJson(onlinetalk::common::PacketType::FileUploadDone, task.request_id, meta, nullptr)) {
    if (error) {
      *error = "failed to send upload done";
    }
    return false;
  }
  return true;
}

bool FileTransferManager::sendDownloadRequest(NetClient& net,
                                              const DownloadTask& task,
                                              uint64_t request_id,
                                              std::string* error) {
  nlohmann::json meta;
  meta["file_id"] = task.file_id;
  meta["offset"] = task.next_offset;
  if (!net.sendJson(onlinetalk::common::PacketType::FileDownloadRequest, request_id, meta, nullptr)) {
    if (error) {
      *error = "failed to send download request";
    }
    return false;
  }
  download_request_map_[request_id] = task.file_id;
  return true;
}

void FileTransferManager::markUploadFailed(const std::string& file_id, const std::string& message) {
  last_error_ = message;
  auto upload_it = uploads_.find(file_id);
  if (upload_it != uploads_.end()) {
    upload_it->second.failed = true;
    upload_it->second.stream.reset();
  }
  auto state_it = upload_states_.find(file_id);
  if (state_it != upload_states_.end()) {
    state_it->second.failed = true;
  }
}

void FileTransferManager::markDownloadFailed(const std::string& file_id, const std::string& message) {
  last_error_ = message;
  auto download_it = downloads_.find(file_id);
  if (download_it != downloads_.end()) {
    download_it->second.failed = true;
  }
  auto state_it = download_states_.find(file_id);
  if (state_it != download_states_.end()) {
    state_it->second.failed = true;
  }
}

void FileTransferManager::eraseUploadMapping(const std::string& file_id) {
  for (auto it = upload_request_map_.begin(); it != upload_request_map_.end();) {
    if (it->second == file_id) {
      it = upload_request_map_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string FileTransferManager::sanitizeFileName(const std::string& name) {
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

std::string FileTransferManager::downloadDir(const std::string& conversation_type,
                                             const std::string& conversation_id) const {
  std::string dir = data_dir_;
  if (!dir.empty() && dir.back() == '/') {
    dir.pop_back();
  }
  dir += "/downloads";
  if (!conversation_type.empty()) {
    dir += "/" + conversation_type;
  }
  if (!conversation_id.empty()) {
    dir += "/" + conversation_id;
  }
  return dir;
}

}  // namespace onlinetalk::client
