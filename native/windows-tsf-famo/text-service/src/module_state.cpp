#include "module_state.h"

#include <atomic>

namespace famo::tsf {

namespace {
HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};
} // namespace

void SetModuleHandle(HMODULE module) { g_module = module; }
HMODULE ModuleHandle() { return g_module; }

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      g_module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  return path;
}

std::wstring ModuleDirectory() {
  std::wstring path = ModulePath();
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

void AddModuleObject() { ++g_objects; }
void RemoveModuleObject() { --g_objects; }
void AddServerLock() { ++g_locks; }
void RemoveServerLock() { --g_locks; }
bool ModuleCanUnload() { return g_objects == 0 && g_locks == 0; }

} // namespace famo::tsf
