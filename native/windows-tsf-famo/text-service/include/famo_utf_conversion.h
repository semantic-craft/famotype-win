#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace famo::tsf {

bool Utf8ToUtf16(std::string_view input, std::wstring *output);

struct Utf16Preedit {
  std::wstring text;
  uint32_t selection_start = 0;
  uint32_t selection_end = 0;
  uint32_t cursor = 0;
};

bool Utf8PreeditToUtf16(std::string_view input, uint32_t selection_start,
                        uint32_t selection_end, uint32_t cursor,
                        Utf16Preedit *output);

} // namespace famo::tsf
