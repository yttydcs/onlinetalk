#include "common/crypto/sha256.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>

namespace onlinetalk::common {

namespace {

std::string digestToHex(const unsigned char* digest, size_t size) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < size; ++i) {
    ss << std::setw(2) << static_cast<int>(digest[i]);
  }
  return ss.str();
}

}  // namespace

std::string sha256Hex(const std::vector<uint8_t>& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  if (!data.empty()) {
    SHA256_Update(&ctx, data.data(), data.size());
  }
  SHA256_Final(digest, &ctx);
  return digestToHex(digest, SHA256_DIGEST_LENGTH);
}

std::string sha256HexFile(const std::string& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "failed to open file: " + path;
    }
    return {};
  }

  SHA256_CTX ctx;
  SHA256_Init(&ctx);

  std::vector<char> buffer(64 * 1024);
  while (file.good()) {
    file.read(buffer.data(), buffer.size());
    const auto read_size = file.gcount();
    if (read_size > 0) {
      SHA256_Update(&ctx, buffer.data(), static_cast<size_t>(read_size));
    }
  }
  if (file.bad()) {
    if (error) {
      *error = "failed while reading file: " + path;
    }
    return {};
  }

  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, &ctx);
  return digestToHex(digest, SHA256_DIGEST_LENGTH);
}

}  // namespace onlinetalk::common
