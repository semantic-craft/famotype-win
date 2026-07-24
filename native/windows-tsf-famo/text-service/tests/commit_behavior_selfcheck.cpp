#include <cstdio>
#include <initializer_list>
#include <vector>

#include "famo_commit_behavior.h"
#include "famo_input_injection.h"
#include "famo_runtime_protocol.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo;

namespace {

struct FakeInjection {
  FakeInjection(WORD held_value, std::initializer_list<UINT> scripted)
      : held(held_value), results(scripted) {
    events.reserve(16);
  }

  WORD held = 0;
  std::vector<UINT> results;
  std::vector<INPUT> events;
  size_t next = 0;

  static SHORT KeyState(int key, void *user) noexcept {
    const auto *self = static_cast<const FakeInjection *>(user);
    return key == self->held ? static_cast<SHORT>(0x8000) : 0;
  }

  static UINT SendOne(const INPUT &input, void *user) noexcept {
    auto *self = static_cast<FakeInjection *>(user);
    self->events.push_back(input);
    return self->next < self->results.size() ? self->results[self->next++] : 1;
  }

  tsf::InputInjectionApi api() { return {&KeyState, &SendOne, this}; }
};

} // namespace

int main() {
  auto pair = tsf::TransformCommit(L"（", 0, 0, runtime::kHostAutoPair);
  CHECK(pair.text == L"（）" && pair.caret_back == 1);
  auto type_over =
      tsf::TransformCommit(L"）", 0, L'）', runtime::kHostAutoPair);
  CHECK(type_over.text.empty() && type_over.advance_over_existing);
  auto english = tsf::TransformCommit(L"A中B", L'文', L'字',
                                      runtime::kHostCjkEnglishSpacing);
  CHECK(english.text == L" A 中 B ");
  auto number = tsf::TransformCommit(L"3中4", L'文', L'字',
                                     runtime::kHostCjkNumberSpacing);
  CHECK(number.text == L" 3 中 4 ");
  auto disabled = tsf::TransformCommit(L"A中", L'文', 0, 0);
  CHECK(disabled.text == L"A中");

  FakeInjection partial_key_up{0, {1, 0, 1}};
  const auto partial_key_up_result =
      tsf::InjectArrowKeys(VK_RIGHT, 1, partial_key_up.api());
  CHECK(partial_key_up_result.moved == 1 && !partial_key_up_result.complete);
  CHECK(partial_key_up.events.size() == 3);
  CHECK(partial_key_up.events[0].ki.wVk == VK_RIGHT &&
        partial_key_up.events[0].ki.dwFlags == 0);
  CHECK(partial_key_up.events[1].ki.dwFlags == KEYEVENTF_KEYUP &&
        partial_key_up.events[2].ki.dwFlags == KEYEVENTF_KEYUP);

  FakeInjection restore_before_fallback{VK_LSHIFT, {1, 0, 1}};
  const auto restore_result =
      tsf::InjectArrowKeys(VK_RIGHT, 1, restore_before_fallback.api());
  CHECK(restore_result.moved == 0 && !restore_result.complete);
  CHECK(restore_before_fallback.events.size() == 3);
  CHECK(restore_before_fallback.events[0].ki.wVk == VK_LSHIFT &&
        restore_before_fallback.events[0].ki.dwFlags == KEYEVENTF_KEYUP);
  CHECK(restore_before_fallback.events[1].ki.wVk == VK_RIGHT);
  CHECK(restore_before_fallback.events[2].ki.wVk == VK_LSHIFT &&
        restore_before_fallback.events[2].ki.dwFlags == 0);

  FakeInjection partial_movement{0, {1, 1, 0}};
  const auto partial_movement_result =
      tsf::InjectArrowKeys(VK_LEFT, 2, partial_movement.api());
  CHECK(partial_movement_result.moved == 1 &&
        !partial_movement_result.complete);

  FakeInjection restore_retry{VK_LSHIFT, {1, 1, 1, 0, 1}};
  const auto restore_retry_result =
      tsf::InjectArrowKeys(VK_LEFT, 1, restore_retry.api());
  CHECK(restore_retry_result.moved == 1 && restore_retry_result.complete);
  CHECK(restore_retry.events.size() == 5);
  std::printf("commit_behavior_selfcheck: OK\n");
  return 0;
}
