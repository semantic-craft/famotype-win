#pragma once

#include <new>
#include <utility>

#include <windows.h>

namespace famo::tsf {

template <typename Function>
HRESULT ComBoundary(Function &&function) noexcept {
  try {
    return std::forward<Function>(function)();
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

template <typename Result, typename Function>
Result BoundaryOr(Result fallback, Function &&function) noexcept {
  try {
    return std::forward<Function>(function)();
  } catch (...) {
    return fallback;
  }
}

} // namespace famo::tsf
