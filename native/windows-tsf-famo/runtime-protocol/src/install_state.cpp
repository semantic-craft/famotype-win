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

bool EqualPath(std::wstring_view left, std::wstring_view right) {
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

bool ActiveRuntimeProjectionAllowed(
    std::wstring_view state, std::wstring_view install_directory,
    std::wstring_view server_executable, bool allow_activating) {
  install_directory = TrimDirectory(install_directory);
  if (!InstallTargetAllowed(state, install_directory, install_directory,
                            allow_activating) ||
      install_directory.size() >
          static_cast<size_t>(INT_MAX) - std::wstring_view(L"\\FamoRuntime.exe").size()) {
    return false;
  }
  try {
    std::wstring expected(install_directory);
    expected += L"\\FamoRuntime.exe";
    return EqualPath(expected, server_executable);
  } catch (...) {
    return false;
  }
}

bool ResolveProductionRuntime(std::wstring *server_executable,
                              bool allow_activating) {
  if (!server_executable)
    return false;
  server_executable->clear();
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Famo\\InputMethod", 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t state[32]{};
  wchar_t install_directory[32768]{};
  wchar_t server[32768]{};
  DWORD state_bytes = sizeof(state);
  DWORD directory_bytes = sizeof(install_directory);
  DWORD server_bytes = sizeof(server);
  const LSTATUS state_result =
      RegGetValueW(key, nullptr, L"InstallState", RRF_RT_REG_SZ, nullptr,
                   state, &state_bytes);
  const LSTATUS directory_result =
      RegGetValueW(key, nullptr, L"InstallDir", RRF_RT_REG_SZ, nullptr,
                   install_directory, &directory_bytes);
  const LSTATUS server_result =
      RegGetValueW(key, nullptr, L"ServerExecutable", RRF_RT_REG_SZ, nullptr,
                   server, &server_bytes);
  RegCloseKey(key);
  if (state_result != ERROR_SUCCESS ||
      directory_result != ERROR_SUCCESS ||
      server_result != ERROR_SUCCESS ||
      !ActiveRuntimeProjectionAllowed(state, install_directory, server,
                                      allow_activating)) {
    return false;
  }
  try {
    server_executable->assign(server);
    return true;
  } catch (...) {
    server_executable->clear();
    return false;
  }
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
