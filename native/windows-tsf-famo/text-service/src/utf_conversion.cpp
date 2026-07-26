#include "famo_utf_conversion.h"

#include <limits>
#include <utility>

#include <windows.h>

namespace famo::tsf {

namespace {

bool Utf8PrefixToUtf16Length(std::string_view input, uint32_t bytes,
                             uint32_t *output) {
  if (!output || bytes > input.size() ||
      bytes > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    return false;
  if (bytes == 0) {
    *output = 0;
    return true;
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       input.data(), static_cast<int>(bytes),
                                       nullptr, 0);
  if (size <= 0)
    return false;
  *output = static_cast<uint32_t>(size);
  return true;
}

} // namespace

bool Utf8ToUtf16(std::string_view input, std::wstring *output) {
  if (!output || input.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;
  output->clear();
  if (input.empty())
    return true;
  const int size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
      nullptr, 0);
  if (size <= 0)
    return false;
  output->resize(static_cast<size_t>(size));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                             static_cast<int>(input.size()), output->data(),
                             size) == size;
}

bool Utf8PreeditToUtf16(std::string_view input, uint32_t selection_start,
                        uint32_t selection_end, uint32_t cursor,
                        Utf16Preedit *output) {
  if (!output || selection_start > selection_end || selection_end > input.size() ||
      cursor > input.size())
    return false;
  Utf16Preedit converted;
  if (!Utf8ToUtf16(input, &converted.text) ||
      !Utf8PrefixToUtf16Length(input, selection_start,
                               &converted.selection_start) ||
      !Utf8PrefixToUtf16Length(input, selection_end,
                               &converted.selection_end) ||
      !Utf8PrefixToUtf16Length(input, cursor, &converted.cursor))
    return false;
  *output = std::move(converted);
  return true;
}

} // namespace famo::tsf
