#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace famo::tsf {

struct CommitBehaviorResult {
  std::wstring text;
  uint32_t caret_back = 0;
  bool advance_over_existing = false;
};

CommitBehaviorResult TransformCommit(std::wstring_view commit, wchar_t before,
                                     wchar_t after, uint32_t behavior_flags);

} // namespace famo::tsf
