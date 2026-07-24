#pragma once

#include "famo_runtime_protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace famo::runtime {

class Writer {
public:
  void U8(uint8_t value) { bytes_.push_back(value); }
  void U16(uint16_t value) {
    U8(static_cast<uint8_t>(value));
    U8(static_cast<uint8_t>(value >> 8));
  }
  void U32(uint32_t value) {
    for (int i = 0; i < 4; ++i)
      U8(static_cast<uint8_t>(value >> (i * 8)));
  }
  void U64(uint64_t value) {
    for (int i = 0; i < 8; ++i)
      U8(static_cast<uint8_t>(value >> (i * 8)));
  }
  void Bytes(std::span<const uint8_t> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  bool String(std::string_view value, std::string *error) {
    if (value.size() > kMaxStringBytes || !IsValidUtf8(value)) {
      if (error)
        *error = "invalid or oversized UTF-8 string";
      return false;
    }
    U32(static_cast<uint32_t>(value.size()));
    Bytes(std::span(reinterpret_cast<const uint8_t *>(value.data()),
                    value.size()));
    return true;
  }
  std::vector<uint8_t> Take() { return std::move(bytes_); }

private:
  std::vector<uint8_t> bytes_;
};

class Reader {
public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  bool U8(uint8_t *value) {
    if (remaining() < 1)
      return false;
    *value = bytes_[offset_++];
    return true;
  }
  bool U16(uint16_t *value) {
    uint8_t a, b;
    if (!U8(&a) || !U8(&b))
      return false;
    *value = static_cast<uint16_t>(a | (static_cast<uint16_t>(b) << 8));
    return true;
  }
  bool U32(uint32_t *value) {
    if (remaining() < 4)
      return false;
    *value = 0;
    for (int i = 0; i < 4; ++i)
      *value |= static_cast<uint32_t>(bytes_[offset_++]) << (i * 8);
    return true;
  }
  bool U64(uint64_t *value) {
    if (remaining() < 8)
      return false;
    *value = 0;
    for (int i = 0; i < 8; ++i)
      *value |= static_cast<uint64_t>(bytes_[offset_++]) << (i * 8);
    return true;
  }
  bool String(std::string &value, std::string *error) {
    uint32_t size = 0;
    if (!U32(&size) || size > kMaxStringBytes || size > remaining()) {
      if (error)
        *error = "truncated or oversized string";
      return false;
    }
    value.assign(reinterpret_cast<const char *>(bytes_.data() + offset_), size);
    offset_ += size;
    if (!IsValidUtf8(value)) {
      if (error)
        *error = "invalid UTF-8";
      return false;
    }
    return true;
  }
  size_t remaining() const { return bytes_.size() - offset_; }
  bool done() const { return offset_ == bytes_.size(); }

private:
  std::span<const uint8_t> bytes_;
  size_t offset_ = 0;
};

inline bool Finish(const Reader &reader, std::string *error) {
  if (reader.done())
    return true;
  if (error)
    *error = "trailing payload bytes";
  return false;
}

} // namespace famo::runtime
