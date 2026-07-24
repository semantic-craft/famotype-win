#include "famo_input_injection.h"

#include <iterator>

namespace famo::tsf {
namespace {

INPUT KeyEvent(WORD key, DWORD flags = 0) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = key;
  input.ki.dwFlags = flags;
  return input;
}

bool SendOne(const InputInjectionApi &api, const INPUT &input) {
  return api.send_one && api.send_one(input, api.user) == 1;
}

bool Compensate(const InputInjectionApi &api, const INPUT &input) {
  if (SendOne(api, input))
    return true;
  return SendOne(api, input);
}

} // namespace

ArrowInjectionResult InjectArrowKeys(WORD key, uint32_t count,
                                     const InputInjectionApi &api) noexcept {
  constexpr WORD kModifiers[] = {VK_LSHIFT,   VK_RSHIFT, VK_LCONTROL,
                                 VK_RCONTROL, VK_LMENU,  VK_RMENU};
  ArrowInjectionResult result;
  if ((key != VK_LEFT && key != VK_RIGHT) || count == 0 || count > 4 ||
      !api.key_state || !api.send_one)
    return result;

  bool released[std::size(kModifiers)]{};
  const auto restore_modifiers = [&]() {
    bool restored = true;
    for (size_t i = 0; i < std::size(kModifiers); ++i) {
      if (released[i] && !Compensate(api, KeyEvent(kModifiers[i])))
        restored = false;
    }
    return restored;
  };

  for (size_t i = 0; i < std::size(kModifiers); ++i) {
    if ((api.key_state(kModifiers[i], api.user) & 0x8000) == 0)
      continue;
    if (!SendOne(api, KeyEvent(kModifiers[i], KEYEVENTF_KEYUP))) {
      restore_modifiers();
      return result;
    }
    released[i] = true;
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (!SendOne(api, KeyEvent(key))) {
      restore_modifiers();
      return result;
    }
    ++result.moved;
    if (!SendOne(api, KeyEvent(key, KEYEVENTF_KEYUP))) {
      Compensate(api, KeyEvent(key, KEYEVENTF_KEYUP));
      restore_modifiers();
      return result;
    }
  }
  result.complete = restore_modifiers();
  return result;
}

} // namespace famo::tsf
