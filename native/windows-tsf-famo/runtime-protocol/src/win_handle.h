#pragma once

#include <memory>

#include <windows.h>

namespace famo::runtime::win {

struct HandleCloser {
  void operator()(void *value) const {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

} // namespace famo::runtime::win
