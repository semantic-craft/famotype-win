#include "famo_user_data_lock.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>

namespace famo::runtime {
namespace {

constexpr ULONGLONG kLockTimeoutMs = 30000;
constexpr DWORD kRetryMs = 25;

struct ThreadLockState {
  HANDLE mutex = nullptr;
  HANDLE directory = INVALID_HANDLE_VALUE;
  HANDLE file = INVALID_HANDLE_VALUE;
  bool mutex_owned = false;
  uint32_t depth = 0;
};

thread_local ThreadLockState g_lock;

void SetError(std::string_view operation, DWORD code, std::string *error) {
  if (!error)
    return;
  try {
    *error = std::string(operation) + " failed: " + std::to_string(code);
  } catch (...) {
  }
}

void CloseThreadLock() noexcept {
  if (g_lock.file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_lock.file);
    g_lock.file = INVALID_HANDLE_VALUE;
  }
  if (g_lock.directory != INVALID_HANDLE_VALUE) {
    CloseHandle(g_lock.directory);
    g_lock.directory = INVALID_HANDLE_VALUE;
  }
  if (g_lock.mutex) {
    if (g_lock.mutex_owned)
      ReleaseMutex(g_lock.mutex);
    CloseHandle(g_lock.mutex);
    g_lock.mutex = nullptr;
    g_lock.mutex_owned = false;
  }
  g_lock.depth = 0;
}

bool CurrentUserSid(std::wstring *sid, std::string *error) {
  if (!sid)
    return false;
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    SetError("OpenProcessToken(user-data lock)", GetLastError(), error);
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(raw_token, TokenUser, nullptr, 0, &size);
  if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    const DWORD code = GetLastError();
    CloseHandle(raw_token);
    SetError("GetTokenInformation(user-data lock size)", code, error);
    return false;
  }
  std::vector<unsigned char> storage(size);
  if (!GetTokenInformation(raw_token, TokenUser, storage.data(), size,
                           &size)) {
    const DWORD code = GetLastError();
    CloseHandle(raw_token);
    SetError("GetTokenInformation(user-data lock)", code, error);
    return false;
  }
  CloseHandle(raw_token);
  const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.data());
  LPWSTR allocated = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &allocated)) {
    SetError("ConvertSidToStringSid(user-data lock)", GetLastError(), error);
    return false;
  }
  sid->assign(allocated);
  LocalFree(allocated);
  return !sid->empty() &&
         sid->find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                L"abcdefghijklmnopqrstuvwxyz"
                                L"0123456789-_") ==
             std::wstring::npos;
}

bool LocalAppData(std::wstring *path, std::string *error) {
  if (!path)
    return false;
  std::wstring configured;
  const DWORD override_size = GetEnvironmentVariableW(
      L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", nullptr, 0);
  if (override_size > 1 && override_size <= 32768) {
    configured.assign(static_cast<size_t>(override_size), L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", configured.data(),
        override_size);
    if (copied == 0 || copied >= override_size)
      return false;
    configured.resize(copied);
  } else {
    PWSTR allocated = nullptr;
    const HRESULT located = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &allocated);
    if (FAILED(located) || !allocated || !*allocated) {
      if (allocated)
        CoTaskMemFree(allocated);
      SetError("SHGetKnownFolderPath(LOCALAPPDATA)",
               FAILED(located) ? HRESULT_CODE(located) : ERROR_PATH_NOT_FOUND,
               error);
      return false;
    }
    configured.assign(allocated);
    CoTaskMemFree(allocated);
  }
  const DWORD full_required =
      GetFullPathNameW(configured.c_str(), 0, nullptr, nullptr);
  if (full_required == 0 || full_required > 32768) {
    SetError("GetFullPathName(LOCALAPPDATA)", GetLastError(), error);
    return false;
  }
  std::wstring full(static_cast<size_t>(full_required), L'\0');
  const DWORD full_size = GetFullPathNameW(
      configured.c_str(), full_required, full.data(), nullptr);
  if (full_size == 0 || full_size >= full_required) {
    SetError("GetFullPathName(LOCALAPPDATA)", GetLastError(), error);
    return false;
  }
  full.resize(full_size);
  *path = std::move(full);
  return true;
}

std::wstring FinalPath(HANDLE handle) {
  std::wstring path(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle, path.data(), static_cast<DWORD>(path.size()),
      FILE_NAME_NORMALIZED);
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  if (path.starts_with(L"\\\\?\\UNC\\"))
    path = L"\\\\" + path.substr(8);
  else if (path.starts_with(L"\\\\?\\"))
    path.erase(0, 4);
  while (path.size() > 3 &&
         (path.back() == L'\\' || path.back() == L'/')) {
    path.pop_back();
  }
  return path;
}

bool ValidatePinnedObject(HANDLE handle, const std::wstring &expected_path,
                          bool expect_directory, std::string *error) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) {
    SetError("GetFileInformationByHandle(user-data lock)", GetLastError(),
             error);
    return false;
  }
  const bool directory =
      (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const uint64_t object_id =
      (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
      information.nFileIndexLow;
  const std::wstring final_path = FinalPath(handle);
  DWORD validation_error = ERROR_SUCCESS;
  if (directory != expect_directory)
    validation_error = ERROR_DIRECTORY;
  else if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    validation_error = ERROR_REPARSE_TAG_INVALID;
  else if (object_id == 0)
    validation_error = ERROR_INVALID_DATA;
  else if (final_path.empty())
    validation_error = ERROR_BAD_PATHNAME;
  else if (_wcsicmp(final_path.c_str(), expected_path.c_str()) != 0)
    validation_error = ERROR_NOT_SAME_DEVICE;
  if (validation_error != ERROR_SUCCESS) {
    SetError(expect_directory
                 ? "ValidatePinnedDirectory(user-data lock)"
                 : "ValidatePinnedFile(user-data lock)",
             validation_error, error);
    return false;
  }
  return true;
}

DWORD OpenOrCreateRelativeDirectory(HANDLE parent, std::wstring_view leaf,
                                    const std::wstring &expected_path,
                                    HANDLE *opened,
                                    std::wstring *opened_path,
                                    std::string *error) {
  if (!opened || leaf.empty() ||
      leaf.size() > USHRT_MAX / sizeof(wchar_t) ||
      leaf.find_first_of(L"\\/") != std::wstring_view::npos ||
      leaf == L"." || leaf == L"..") {
    return ERROR_INVALID_PARAMETER;
  }
  *opened = INVALID_HANDLE_VALUE;
  UNICODE_STRING name{};
  name.Buffer = const_cast<PWSTR>(leaf.data());
  name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, parent,
                             nullptr);
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = NtCreateFile(
      opened, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE |
                  SYNCHRONIZE,
      &attributes, &status_block, nullptr, FILE_ATTRIBUTE_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
      FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT |
          FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);
  if (status < 0) {
    *opened = INVALID_HANDLE_VALUE;
    return RtlNtStatusToDosError(status);
  }
  const std::wstring final_path = FinalPath(*opened);
  if (!ValidatePinnedObject(*opened, expected_path, true, error)) {
    CloseHandle(*opened);
    *opened = INVALID_HANDLE_VALUE;
    return ERROR_BAD_PATHNAME;
  }
  if (opened_path)
    *opened_path = final_path;
  return ERROR_SUCCESS;
}

DWORD OpenRelativeLockFile(HANDLE parent, std::wstring_view leaf,
                           const std::wstring &expected_path,
                           HANDLE *opened, std::string *error) {
  if (!opened || leaf.empty() ||
      leaf.size() > USHRT_MAX / sizeof(wchar_t) ||
      leaf.find_first_of(L"\\/") != std::wstring_view::npos ||
      leaf == L"." || leaf == L"..") {
    return ERROR_INVALID_PARAMETER;
  }
  *opened = INVALID_HANDLE_VALUE;
  UNICODE_STRING name{};
  name.Buffer = const_cast<PWSTR>(leaf.data());
  name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, parent,
                             nullptr);
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = NtCreateFile(
      opened,
      FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      &attributes, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, 0,
      FILE_OPEN_IF,
      FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT |
          FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);
  if (status < 0) {
    *opened = INVALID_HANDLE_VALUE;
    return RtlNtStatusToDosError(status);
  }
  if (!ValidatePinnedObject(*opened, expected_path, false, error)) {
    CloseHandle(*opened);
    *opened = INVALID_HANDLE_VALUE;
    return ERROR_BAD_PATHNAME;
  }
  return ERROR_SUCCESS;
}

} // namespace

UserDataTransactionLock::~UserDataTransactionLock() { Release(); }

bool UserDataTransactionLock::Acquire(std::string *error) noexcept {
  if (held_)
    return true;
  if (g_lock.depth != 0) {
    ++g_lock.depth;
    held_ = true;
    return true;
  }

  try {
    std::wstring sid;
    std::wstring local_data;
    if (!CurrentUserSid(&sid, error) || !LocalAppData(&local_data, error))
      return false;

    const ULONGLONG deadline = GetTickCount64() + kLockTimeoutMs;
    const std::wstring mutex_name =
        L"Global\\Famo.Settings.UserData.Transaction." + sid;
    g_lock.mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (g_lock.mutex) {
      const ULONGLONG now = GetTickCount64();
      const DWORD remaining =
          now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
      const DWORD waited = WaitForSingleObject(g_lock.mutex, remaining);
      if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        const DWORD code = waited == WAIT_TIMEOUT
                               ? ERROR_TIMEOUT
                               : (waited == WAIT_FAILED ? GetLastError()
                                                       : ERROR_GEN_FAILURE);
        SetError("WaitForSingleObject(user-data mutex)", code, error);
        CloseThreadLock();
        return false;
      }
      g_lock.mutex_owned = true;
    } else {
      const DWORD code = GetLastError();
      if (code != ERROR_ACCESS_DENIED && code != ERROR_INVALID_HANDLE) {
        SetError("CreateMutex(user-data lock)", code, error);
        CloseThreadLock();
        return false;
      }
    }
    // ACCESS_DENIED/INVALID_HANDLE mean the Global namespace is unavailable.
    // Only those cases fall back to the still-mandatory per-SID file lock.

    while (!local_data.empty() &&
           (local_data.back() == L'\\' || local_data.back() == L'/')) {
      local_data.pop_back();
    }
    const std::wstring pinned_local_data =
        local_data.starts_with(L"\\\\?\\")
            ? local_data
            : (local_data.starts_with(L"\\\\")
                   ? L"\\\\?\\UNC\\" + local_data.substr(2)
                   : L"\\\\?\\" + local_data);
    g_lock.directory = CreateFileW(
        pinned_local_data.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (g_lock.directory == INVALID_HANDLE_VALUE) {
      SetError("CreateFile(LOCALAPPDATA)", GetLastError(), error);
      CloseThreadLock();
      return false;
    }
    if (!ValidatePinnedObject(g_lock.directory, local_data, true, error)) {
      CloseThreadLock();
      return false;
    }
    const std::wstring directory = local_data + L"\\Famo.UserDataLocks";
    HANDLE lock_directory = INVALID_HANDLE_VALUE;
    std::wstring lock_directory_path;
    const DWORD directory_error = OpenOrCreateRelativeDirectory(
        g_lock.directory, L"Famo.UserDataLocks", directory,
        &lock_directory, &lock_directory_path, error);
    if (directory_error != ERROR_SUCCESS) {
      if (error && error->empty())
        SetError("NtCreateFile(user-data lock directory)", directory_error,
                 error);
      CloseThreadLock();
      return false;
    }
    CloseHandle(g_lock.directory);
    g_lock.directory = lock_directory;

    const std::wstring leaf = sid + L".transaction.lock";
    const std::wstring path =
        (lock_directory_path.empty() ? directory : lock_directory_path) +
        L"\\" + leaf;
    for (;;) {
      const DWORD code = OpenRelativeLockFile(
          g_lock.directory, leaf, path, &g_lock.file, error);
      if (code == ERROR_SUCCESS)
        break;
      if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) {
        if (error && error->empty())
          SetError("NtCreateFile(user-data lock)", code, error);
        CloseThreadLock();
        return false;
      }
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
        SetError("CreateFile(user-data lock)", ERROR_TIMEOUT, error);
        CloseThreadLock();
        return false;
      }
      const DWORD remaining = static_cast<DWORD>(deadline - now);
      Sleep((std::min)(remaining, kRetryMs));
    }
    g_lock.depth = 1;
    held_ = true;
    return true;
  } catch (...) {
    SetError("Acquire(user-data lock)", ERROR_NOT_ENOUGH_MEMORY, error);
    CloseThreadLock();
    return false;
  }
}

void UserDataTransactionLock::Release() noexcept {
  if (!held_)
    return;
  held_ = false;
  if (g_lock.depth == 0)
    return;
  --g_lock.depth;
  if (g_lock.depth == 0)
    CloseThreadLock();
}

} // namespace famo::runtime
