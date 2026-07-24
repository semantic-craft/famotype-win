#include "famo_commit_behavior.h"

#include <array>
#include <utility>

#include "famo_runtime_protocol.h"

namespace famo::tsf {
namespace {

bool IsCjk(wchar_t value) {
  return (value >= 0x3400 && value <= 0x9fff) ||
         (value >= 0xf900 && value <= 0xfaff);
}

bool IsEnglish(wchar_t value) {
  return (value >= L'a' && value <= L'z') ||
         (value >= L'A' && value <= L'Z');
}

bool IsNumber(wchar_t value) { return value >= L'0' && value <= L'9'; }

bool NeedsSpace(wchar_t left, wchar_t right, uint32_t flags) {
  const bool english = (flags & runtime::kHostCjkEnglishSpacing) != 0;
  const bool number = (flags & runtime::kHostCjkNumberSpacing) != 0;
  return (IsCjk(left) && ((english && IsEnglish(right)) ||
                          (number && IsNumber(right)))) ||
         (IsCjk(right) && ((english && IsEnglish(left)) ||
                           (number && IsNumber(left))));
}

wchar_t ClosingPair(wchar_t opening) {
  constexpr std::array pairs = {
      std::pair{L'(', L')'}, std::pair{L'[', L']'}, std::pair{L'{', L'}'},
      std::pair{L'（', L'）'}, std::pair{L'【', L'】'},
      std::pair{L'《', L'》'}, std::pair{L'〈', L'〉'},
      std::pair{L'「', L'」'}, std::pair{L'『', L'』'},
      std::pair{L'〔', L'〕'}, std::pair{L'［', L'］'},
      std::pair{L'｛', L'｝'}, std::pair{L'“', L'”'},
      std::pair{L'‘', L'’'}};
  for (const auto &[left, right] : pairs) {
    if (opening == left)
      return right;
  }
  return 0;
}

bool IsClosingPair(wchar_t value) {
  constexpr std::wstring_view closings = L")]｝】》〉」』〕］）}”’";
  return closings.find(value) != std::wstring_view::npos;
}

} // namespace

CommitBehaviorResult TransformCommit(std::wstring_view commit, wchar_t before,
                                     wchar_t after, uint32_t flags) {
  CommitBehaviorResult result;
  if (commit.empty())
    return result;
  if ((flags & runtime::kHostAutoPair) != 0 && commit.size() == 1 &&
      IsClosingPair(commit.front()) && after == commit.front()) {
    result.advance_over_existing = true;
    return result;
  }

  result.text.reserve(commit.size() + 3);
  for (wchar_t value : commit) {
    if (!result.text.empty() && result.text.back() != L' ' && value != L' ' &&
        NeedsSpace(result.text.back(), value, flags))
      result.text.push_back(L' ');
    result.text.push_back(value);
  }
  if (before != 0 && before != L' ' && result.text.front() != L' ' &&
      NeedsSpace(before, result.text.front(), flags))
    result.text.insert(result.text.begin(), L' ');
  if (after != 0 && after != L' ' && result.text.back() != L' ' &&
      NeedsSpace(result.text.back(), after, flags))
    result.text.push_back(L' ');

  if ((flags & runtime::kHostAutoPair) != 0) {
    const wchar_t closing = ClosingPair(result.text.back());
    if (closing != 0) {
      result.text.push_back(closing);
      result.caret_back = 1;
    }
  }
  return result;
}

} // namespace famo::tsf
