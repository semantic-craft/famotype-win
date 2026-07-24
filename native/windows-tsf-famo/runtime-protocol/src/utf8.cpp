#include "famo_runtime_protocol.h"

namespace famo::runtime {

bool IsValidUtf8(std::string_view text) {
  const auto *p = reinterpret_cast<const uint8_t *>(text.data());
  size_t i = 0;
  while (i < text.size()) {
    uint32_t code_point = 0;
    size_t count = 0;
    const uint8_t first = p[i];
    if (first <= 0x7f) {
      code_point = first;
      count = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      count = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      count = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      count = 4;
    } else {
      return false;
    }
    if (i + count > text.size())
      return false;
    for (size_t j = 1; j < count; ++j) {
      if ((p[i + j] & 0xc0) != 0x80)
        return false;
      code_point = (code_point << 6) | (p[i + j] & 0x3f);
    }
    if ((count == 3 && code_point < 0x800) ||
        (count == 4 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    i += count;
  }
  return true;
}

} // namespace famo::runtime
