#pragma once

#include <cstdint>

#include <windows.h>

namespace famo::tsf {

struct ArrowInjectionResult {
  uint32_t moved = 0;
  bool complete = false;
};

struct InputInjectionApi {
  SHORT (*key_state)(int key, void *user) noexcept = nullptr;
  UINT (*send_one)(const INPUT &input, void *user) noexcept = nullptr;
  void *user = nullptr;
};

ArrowInjectionResult InjectArrowKeys(WORD key, uint32_t count,
                                     const InputInjectionApi &api) noexcept;

} // namespace famo::tsf
