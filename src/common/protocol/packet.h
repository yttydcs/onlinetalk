#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onlinetalk::common {

enum class PacketType : uint16_t {
  AuthRegister = 1,
  AuthLogin = 2,
  AuthOk = 3,
  AuthError = 4,
  UserListUpdate = 5,
  PresenceUpdate = 6,
  GroupCreate = 7,
  GroupJoin = 8,
  GroupLeave = 9,
  GroupAdmin = 10,
  MessageSend = 11,
  MessageDeliver = 12,
  HistoryFetch = 13,
  HistoryResponse = 14,
  FileOffer = 15,
  FileAccept = 16,
  FileUploadChunk = 17,
  FileUploadDone = 18,
  FileDownloadRequest = 19,
  FileDownloadChunk = 20,
  FileDone = 21
};

struct PacketHeader {
  static constexpr uint32_t kMagic = 0x4F4C544B;  // "OLTK"
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t type = 0;
  uint32_t flags = 0;
  uint64_t request_id = 0;
  uint32_t meta_len = 0;
  uint32_t bin_len = 0;
};

struct Packet {
  PacketHeader header;
  std::string meta_json;
  std::vector<uint8_t> binary;
};

}  // namespace onlinetalk::common
