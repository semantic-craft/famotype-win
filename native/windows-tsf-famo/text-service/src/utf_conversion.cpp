#include "famo_utf_conversion.h"

#include <limits>

#include <windows.h>

namespace famo::tsf {

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

} // namespace famo::tsf
