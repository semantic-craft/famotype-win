#pragma once

#include <new>
#include <string>
#include <utility>

#include <windows.h>

namespace famo::runtime {

template <typename Function>
bool ProtocolBoundary(std::string *error, Function &&function) noexcept {
  try {
    if (GetEnvironmentVariableA("FAMO_TEST_PROTOCOL_ALLOCATION_FAILURE",
                                nullptr, 0) != 0) {
      throw std::bad_alloc();
    }
    return std::forward<Function>(function)();
  } catch (...) {
    try {
      if (error)
        *error = "protocol allocation failure";
    } catch (...) {
    }
    return false;
  }
}

} // namespace famo::runtime
