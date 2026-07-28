#include "famo_install_state.h"

#include <climits>

#include <windows.h>

namespace famo::runtime {
namespace {

std::wstring_view TrimDirectory(std::wstring_view value) {
  while (value.size() > 1 && (value.back() == L'\\' || value.back() == L'/'))
    value.remove_suffix(1);
  return value;
}

bool EqualDirectory(std::wstring_view left, std::wstring_view right) {
  left = TrimDirectory(left);
  right = TrimDirectory(right);
  if (left.empty() || right.empty() ||
      left.size() > static_cast<size_t>(INT_MAX) ||
      right.size() > static_cast<size_t>(INT_MAX)) {
    return false;
  }
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

} // namespace

bool InstallTargetAllowed(std::wstring_view state,
                          std::wstring_view install_directory,
                          std::wstring_view module_directory,
                          bool allow_activating) {
  const bool state_allowed =
      state == L"Ready" || (allow_activating && state == L"Activating");
  return state_allowed && EqualDirectory(install_directory, module_directory);
}

bool ProductionInstallAllowed(std::wstring_view module_directory,
                              bool allow_activating) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Famo\\InputMethod", 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t state[32]{};
  wchar_t install_directory[32768]{};
  DWORD state_bytes = sizeof(state);
  DWORD directory_bytes = sizeof(install_directory);
  const LSTATUS state_result =
      RegGetValueW(key, nullptr, L"InstallState", RRF_RT_REG_SZ, nullptr,
                   state, &state_bytes);
  const LSTATUS directory_result =
      RegGetValueW(key, nullptr, L"InstallDir", RRF_RT_REG_SZ, nullptr,
                   install_directory, &directory_bytes);
  RegCloseKey(key);
  return state_result == ERROR_SUCCESS && directory_result == ERROR_SUCCESS &&
         InstallTargetAllowed(state, install_directory, module_directory,
                              allow_activating);
}

} // namespace famo::runtime
