#include "common/protocol/codec.h"

#include <array>

namespace onlinetalk::common {

namespace {

void writeU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void writeU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void writeU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t readU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint64_t readU64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

}  // namespace

std::vector<uint8_t> Codec::encode(const Packet& packet) {
  std::vector<uint8_t> out;
  out.reserve(kHeaderSize + packet.meta_json.size() + packet.binary.size());

  writeU32(out, packet.header.magic);
  writeU16(out, packet.header.version);
  writeU16(out, packet.header.type);
  writeU32(out, packet.header.flags);
  writeU64(out, packet.header.request_id);
  writeU32(out, static_cast<uint32_t>(packet.meta_json.size()));
  writeU32(out, static_cast<uint32_t>(packet.binary.size()));

  out.insert(out.end(), packet.meta_json.begin(), packet.meta_json.end());
  out.insert(out.end(), packet.binary.begin(), packet.binary.end());
  return out;
}

bool Codec::decode(ByteBuffer& buffer, Packet* out_packet) {
  if (!out_packet) {
    return false;
  }
  if (buffer.size() < kHeaderSize) {
    return false;
  }

  const uint8_t* data = buffer.data();
  PacketHeader header;
  header.magic = readU32(data);
  header.version = readU16(data + 4);
  header.type = readU16(data + 6);
  header.flags = readU32(data + 8);
  header.request_id = readU64(data + 12);
  header.meta_len = readU32(data + 20);
  header.bin_len = readU32(data + 24);

  if (header.magic != PacketHeader::kMagic || header.version != PacketHeader::kVersion) {
    return false;
  }
  if (header.meta_len > kMaxMetaSize || header.bin_len > kMaxBinarySize) {
    return false;
  }

  const size_t total_size = kHeaderSize + header.meta_len + header.bin_len;
  if (buffer.size() < total_size) {
    return false;
  }

  Packet packet;
  packet.header = header;
  if (header.meta_len > 0) {
    packet.meta_json.assign(reinterpret_cast<const char*>(data + kHeaderSize), header.meta_len);
  }
  if (header.bin_len > 0) {
    packet.binary.assign(data + kHeaderSize + header.meta_len,
                         data + kHeaderSize + header.meta_len + header.bin_len);
  }
  buffer.consume(total_size);
  *out_packet = std::move(packet);
  return true;
}

}  // namespace onlinetalk::common
