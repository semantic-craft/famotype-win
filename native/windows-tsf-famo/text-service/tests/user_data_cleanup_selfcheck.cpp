#define FAMO_PROFILE_TOOL_NO_MAIN
#include "../tools/dev_profile_main.cpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

std::wstring g_profile;
std::wstring g_parked;
std::wstring g_outside;
bool g_hook_attempted = false;
bool g_ancestor_blocked = false;
bool g_ancestor_swapped = false;
std::atomic_bool g_locked_delete_entered = false;
std::wstring g_lock_directory;
std::wstring g_lock_directory_parked;
std::wstring g_lock_directory_outside;
bool g_lock_swap_attempted = false;
bool g_lock_swap_blocked = false;
bool g_lock_swap_succeeded = false;

bool WriteProbeFile(const std::wstring &path, const char *text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(std::strlen(text));
  const bool success =
      WriteFile(file, text, size, &written, nullptr) && written == size;
  CloseHandle(file);
  return success;
}

void AttemptAncestorSwap(const std::wstring &) {
  g_hook_attempted = true;
  if (!MoveFileExW(g_profile.c_str(), g_parked.c_str(), 0)) {
    g_ancestor_blocked = true;
    return;
  }
  g_ancestor_swapped = true;
  if (!CreateSymbolicLinkW(
          g_profile.c_str(), g_outside.c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    MoveFileExW(g_parked.c_str(), g_profile.c_str(), 0);
    g_ancestor_swapped = false;
    g_ancestor_blocked = true;
  }
}

void MarkLockedDeleteEntry(const std::wstring &) {
  g_locked_delete_entered.store(true);
}

void AttemptLockDirectorySwap(const std::wstring &) {
  g_lock_swap_attempted = true;
  if (!MoveFileExW(g_lock_directory.c_str(),
                   g_lock_directory_parked.c_str(), 0)) {
    g_lock_swap_blocked = true;
    return;
  }
  g_lock_swap_succeeded = true;
  if (!CreateSymbolicLinkW(
          g_lock_directory.c_str(), g_lock_directory_outside.c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    MoveFileExW(g_lock_directory_parked.c_str(),
                g_lock_directory.c_str(), 0);
    g_lock_swap_succeeded = false;
    g_lock_swap_blocked = true;
  }
}

bool SettingsCompatibleLockDirectoryResistsSwap(
    const std::wstring &local_data, const std::wstring &root) {
  g_lock_directory = local_data + L"\\Famo.UserDataLocks";
  g_lock_directory_parked = g_lock_directory + L".parked";
  g_lock_directory_outside = root + L"\\outside-lock";
  std::error_code ignored;
  if (!std::filesystem::create_directories(
          g_lock_directory_outside, ignored) ||
      !WriteProbeFile(
          g_lock_directory_outside + L"\\sentinel.txt", "outside"))
    return false;
  std::wstring canonical_sid;
  if (FAILED(CurrentProcessCanonicalSid(&canonical_sid)))
    return false;
  g_lock_swap_attempted = false;
  g_lock_swap_blocked = false;
  g_lock_swap_succeeded = false;
  g_user_data_lock_directory_validation_hook =
      AttemptLockDirectorySwap;
  UserDataTransactionLease lease;
  const HRESULT result = AcquireUserDataTransactionLock(
      local_data, canonical_sid, &lease);
  g_user_data_lock_directory_validation_hook = nullptr;
  lease.Release();
  if (g_lock_swap_succeeded) {
    RemoveDirectoryW(g_lock_directory.c_str());
    if (!MoveFileExW(g_lock_directory_parked.c_str(),
                     g_lock_directory.c_str(), 0))
      return false;
  }
  size_t outside_entries = 0;
  bool outside_sentinel = false;
  for (const auto &entry :
       std::filesystem::directory_iterator(g_lock_directory_outside)) {
    ++outside_entries;
    outside_sentinel =
        outside_sentinel ||
        entry.path().filename() == L"sentinel.txt";
  }
  return SUCCEEDED(result) && g_lock_swap_attempted &&
         g_lock_swap_blocked && !g_lock_swap_succeeded &&
         outside_entries == 1 && outside_sentinel;
}

bool SettingsCompatibleLockBlocksDeletion(
    const std::wstring &delete_parent, const std::wstring &target,
    const std::wstring &nested) {
  std::error_code ignored;
  if (!std::filesystem::create_directories(nested, ignored) ||
      !WriteProbeFile(nested + L"\\locked-payload.txt", "payload"))
    return false;

  // Use the selfcheck tree as an explicit LocalAppData root. Production passes
  // SHGetKnownFolderPath; the test must never leave a real per-user lock file.
  const std::wstring local_data = delete_parent;
  std::wstring canonical_sid;
  if (FAILED(CurrentProcessCanonicalSid(&canonical_sid)))
    return false;

  UserDataTransactionLease settings_lock;
  if (FAILED(AcquireUserDataTransactionLock(
          local_data, canonical_sid, &settings_lock)))
    return false;

  std::atomic_bool worker_started = false;
  HRESULT delete_result = E_FAIL;
  g_locked_delete_entered.store(false);
  g_user_data_delete_validation_hook = MarkLockedDeleteEntry;
  std::thread delete_thread([&] {
    worker_started.store(true);
    UserDataTransactionLease delete_lock;
    delete_result = AcquireUserDataTransactionLock(
        local_data, canonical_sid, &delete_lock);
    if (SUCCEEDED(delete_result))
      delete_result =
          DeletePinnedDirectoryChild(delete_parent, L"Famo");
  });

  const ULONGLONG start_deadline = GetTickCount64() + 2000;
  while (!worker_started.load() && GetTickCount64() < start_deadline)
    Sleep(10);
  Sleep(250);
  const bool blocked =
      worker_started.load() && !g_locked_delete_entered.load() &&
      std::filesystem::exists(target);
  settings_lock.Release();
  delete_thread.join();
  g_user_data_delete_validation_hook = nullptr;
  return blocked && g_locked_delete_entered.load() &&
         SUCCEEDED(delete_result) && !std::filesystem::exists(target);
}

bool ScheduledTaskCompletionRejectsPreStartDefaultResult() {
  bool observed_this_run = false;
  if (ScheduledTaskCompletionCanBeSampled(false, &observed_this_run) ||
      observed_this_run) {
    return false;
  }
  if (ScheduledTaskCompletionCanBeSampled(true, &observed_this_run) ||
      !observed_this_run) {
    return false;
  }
  if (!ScheduledTaskCompletionCanBeSampled(true, &observed_this_run))
    return false;

  DWORD exit_code = STILL_ACTIVE;
  if (TryAcceptScheduledTaskCompletion(
          TASK_STATE_READY, 0, false, &exit_code) ||
      exit_code != STILL_ACTIVE) {
    return false;
  }
  for (const TASK_STATE invalid_state :
       {TASK_STATE_UNKNOWN, TASK_STATE_DISABLED, TASK_STATE_QUEUED,
        TASK_STATE_RUNNING}) {
    if (TryAcceptScheduledTaskCompletion(
            invalid_state, 0, true, &exit_code) ||
        exit_code != STILL_ACTIVE) {
      return false;
    }
  }
  if (!TryAcceptScheduledTaskCompletion(
          TASK_STATE_READY, 1, true, &exit_code) ||
      exit_code != 1) {
    return false;
  }
  exit_code = STILL_ACTIVE;
  return TryAcceptScheduledTaskCompletion(
             TASK_STATE_READY, 0, true, &exit_code) &&
         exit_code == 0;
}

}  // namespace

int wmain() {
  if (!ScheduledTaskCompletionRejectsPreStartDefaultResult())
    return 6;
  wchar_t temporary[MAX_PATH]{};
  if (!GetTempPathW(ARRAYSIZE(temporary), temporary))
    return 1;
  const std::wstring root =
      std::wstring(temporary) + L"famo-user-cleanup-selfcheck-" +
      std::to_wstring(GetCurrentProcessId());
  g_profile = root + L"\\profile";
  g_parked = root + L"\\profile.parked";
  g_outside = root + L"\\outside";
  const std::wstring local = g_profile + L"\\AppData\\Local";
  const std::wstring target = local + L"\\Famo";
  const std::wstring nested = target + L"\\nested";
  const std::wstring outside_target =
      g_outside + L"\\AppData\\Local\\Famo";
  const std::wstring outside_sentinel =
      outside_target + L"\\sentinel.txt";

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  if (!std::filesystem::create_directories(nested, ignored) ||
      !std::filesystem::create_directories(outside_target, ignored) ||
      !WriteProbeFile(nested + L"\\payload.txt", "payload") ||
      !WriteProbeFile(outside_sentinel, "outside"))
    return 2;
  if (!SetFileAttributesW(
          (nested + L"\\payload.txt").c_str(),
          FILE_ATTRIBUTE_READONLY) ||
      !SetFileAttributesW(nested.c_str(), FILE_ATTRIBUTE_READONLY))
    return 2;

  g_user_data_delete_validation_hook = AttemptAncestorSwap;
  const HRESULT result = DeletePinnedDirectoryChild(local, L"Famo");
  g_user_data_delete_validation_hook = nullptr;

  if (g_ancestor_swapped) {
    RemoveDirectoryW(g_profile.c_str());
    if (!MoveFileExW(g_parked.c_str(), g_profile.c_str(), 0))
      return 3;
  }
  const bool ancestor_passed =
      SUCCEEDED(result) && g_hook_attempted &&
      (g_ancestor_blocked || g_ancestor_swapped) &&
      !std::filesystem::exists(target) &&
      std::filesystem::exists(outside_sentinel);
  if (!ancestor_passed) {
    std::filesystem::remove_all(root, ignored);
    return 4;
  }
  const bool lock_passed =
      SettingsCompatibleLockDirectoryResistsSwap(local, root) &&
      SettingsCompatibleLockBlocksDeletion(local, target, nested);
  std::filesystem::remove_all(root, ignored);
  if (!lock_passed)
    return 5;
  std::wprintf(L"user_data_cleanup_selfcheck=ok\n");
  return 0;
}
