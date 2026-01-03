#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/net/byte_buffer.h"
#include "common/protocol/packet.h"

namespace onlinetalk::common {

class Codec {
 public:
  static std::vector<uint8_t> encode(const Packet& packet);
  static bool decode(ByteBuffer& buffer, Packet* out_packet);

  static constexpr size_t kHeaderSize = 28;
  static constexpr uint32_t kMaxMetaSize = 1024 * 1024;
  static constexpr uint32_t kMaxBinarySize = 32 * 1024 * 1024;
};

}  // namespace onlinetalk::common
