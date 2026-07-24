#pragma once

#include <string>
#include <string_view>

namespace famo::tsf {

bool Utf8ToUtf16(std::string_view input, std::wstring *output);

} // namespace famo::tsf
