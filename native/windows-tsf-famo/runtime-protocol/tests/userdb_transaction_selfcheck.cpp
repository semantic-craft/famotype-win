#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <winioctl.h>

#include "famo_runtime_service.h"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #condition,          \
                   __FILE__, __LINE__);                                        \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace {

class TestDirectory {
public:
  explicit TestDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

class TestSink final : public RuntimeSnapshotSink {
public:
  void Publish(std::shared_ptr<const RuntimeSnapshot>) noexcept override {}
};

std::string Utf8(const std::filesystem::path &path) {
  const std::wstring wide = path.wstring();
  const int required =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                          static_cast<int>(wide.size()), nullptr, 0, nullptr,
                          nullptr);
  if (required <= 0)
    return {};
  std::string value(static_cast<size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                          static_cast<int>(wide.size()), value.data(),
                          required, nullptr, nullptr) != required) {
    return {};
  }
  return value;
}

std::wstring Quote(std::wstring_view value) {
  std::wstring quoted(1, L'"');
  size_t backslashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool PrepareRoot(const std::filesystem::path &root) {
  std::error_code error;
  std::filesystem::create_directories(root / "rime_ice.userdb", error);
  if (error)
    return false;
  std::filesystem::create_directories(root / "wubi86_jidian.userdb", error);
  if (error)
    return false;
  std::ofstream(root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\n";
  std::ofstream(root / "famo-select-schema.txt") << "test\n";
  std::ofstream(root / "rime_ice.userdb" / "CURRENT") << "rime-marker";
  std::ofstream(root / "wubi86_jidian.userdb" / "CURRENT") << "wubi-marker";
  return std::filesystem::exists(root / "famo-options.yaml") &&
         std::filesystem::exists(root / "famo-select-schema.txt") &&
         std::filesystem::exists(root / "rime_ice.userdb" / "CURRENT") &&
         std::filesystem::exists(root / "wubi86_jidian.userdb" / "CURRENT");
}

std::string ReadText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool CreateJunction(const std::filesystem::path &junction,
                    const std::filesystem::path &target) {
  wchar_t full_target[32768]{};
  const DWORD length =
      GetFullPathNameW(target.c_str(), static_cast<DWORD>(std::size(full_target)),
                       full_target, nullptr);
  if (length == 0 || length >= std::size(full_target))
    return false;
  const std::wstring print_name(full_target, length);
  const std::wstring substitute_name = L"\\??\\" + print_name;
  if (substitute_name.size() > 4000 || print_name.size() > 4000)
    return false;
  if (!CreateDirectoryW(junction.c_str(), nullptr))
    return false;
  HANDLE directory = CreateFileW(
      junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(junction.c_str());
    return false;
  }
  struct JunctionBuffer {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t path[8192];
  } buffer{};
  buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer.substitute_offset = 0;
  buffer.substitute_length =
      static_cast<WORD>(substitute_name.size() * sizeof(wchar_t));
  buffer.print_offset =
      static_cast<WORD>(buffer.substitute_length + sizeof(wchar_t));
  buffer.print_length =
      static_cast<WORD>(print_name.size() * sizeof(wchar_t));
  std::memcpy(buffer.path, substitute_name.data(), buffer.substitute_length);
  std::memcpy(reinterpret_cast<unsigned char *>(buffer.path) +
                  buffer.print_offset,
              print_name.data(), buffer.print_length);
  const DWORD path_bytes =
      buffer.print_offset + buffer.print_length + sizeof(wchar_t);
  buffer.data_length = static_cast<WORD>(
      sizeof(buffer.substitute_offset) + sizeof(buffer.substitute_length) +
      sizeof(buffer.print_offset) + sizeof(buffer.print_length) + path_bytes);
  const DWORD input_size =
      static_cast<DWORD>(offsetof(JunctionBuffer, path) + path_bytes);
  DWORD transferred = 0;
  const bool created =
      DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &buffer, input_size,
                      nullptr, 0, &transferred, nullptr) != FALSE;
  CloseHandle(directory);
  if (!created)
    RemoveDirectoryW(junction.c_str());
  return created;
}

bool ReplaceJunction(const std::filesystem::path &junction,
                     const std::filesystem::path &target) {
  return RemoveDirectoryW(junction.c_str()) && CreateJunction(junction, target);
}

int Child(const std::filesystem::path &root,
          const std::filesystem::path &lock_root,
          std::wstring_view crash_phase) {
  if (_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA",
                 lock_root.c_str()) != 0 ||
      _wputenv_s(L"FAMO_TEST_USERDB_CRASH_PHASE",
                 std::wstring(crash_phase).c_str()) != 0) {
    return 2;
  }
  const std::string root_utf8 = Utf8(root);
  if (root_utf8.empty())
    return 3;
  RuntimeService service;
  TestSink sink;
  service.SetSnapshotSink(&sink);
  std::string error;
  if (!service.Start(L"FamoTestEngine.dll", root_utf8.c_str(), &error))
    return 4;
  if (service.InitializeControlState() != ControlError::None)
    return 5;
  (void)service.ExecuteControl(Command::ControlResetUserDictionary);
  // Every requested phase terminates inside ResetUserDictionary.
  return 6;
}

bool LaunchCrashChild(const std::filesystem::path &root,
                      const std::filesystem::path &lock_root,
                      std::wstring_view crash_phase) {
  std::wstring executable(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size())
    return false;
  executable.resize(length);
  std::wstring command = Quote(executable) + L" --child " +
                         Quote(root.wstring()) + L" " +
                         Quote(lock_root.wstring()) + L" " +
                         Quote(crash_phase);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, 15000);
  DWORD exit_code = 0;
  const bool exited =
      waited == WAIT_OBJECT_0 &&
      GetExitCodeProcess(process.hProcess, &exit_code) &&
      exit_code == 197;
  if (waited == WAIT_TIMEOUT)
    TerminateProcess(process.hProcess, 198);
  CloseHandle(process.hProcess);
  return exited;
}

bool HasMarker(const std::filesystem::path &root,
               std::string_view marker) {
  const std::filesystem::path backup = root / ".famo-backup";
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(backup, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_directory(error) &&
        std::filesystem::exists(iterator->path() / marker, error)) {
      return true;
    }
  }
  return false;
}

std::filesystem::path FindTransaction(const std::filesystem::path &root) {
  const std::filesystem::path backup = root / ".famo-backup";
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(backup, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_directory(error) &&
        std::filesystem::exists(iterator->path() /
                                    "famo-userdb.pending",
                                error)) {
      return iterator->path();
    }
  }
  return {};
}

int RunPhase(std::wstring_view phase, bool committed) {
  const std::filesystem::path unique =
      std::filesystem::temp_directory_path() /
      ("famo-userdb-crash-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-" +
       std::to_string(static_cast<unsigned long long>(
           std::chrono::steady_clock::now().time_since_epoch().count())));
  TestDirectory cleanup(unique);
  const std::filesystem::path root = unique / "data";
  const std::filesystem::path lock_root = unique / "local-app-data";
  std::filesystem::create_directories(lock_root);
  CHECK(PrepareRoot(root));
  CHECK(LaunchCrashChild(root, lock_root, phase));

  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA",
                   lock_root.c_str()) == 0);
  const std::string root_utf8 = Utf8(root);
  CHECK(!root_utf8.empty());
  RuntimeService recovered;
  TestSink sink;
  recovered.SetSnapshotSink(&sink);
  std::string error;
  CHECK(recovered.Start(L"FamoTestEngine.dll", root_utf8.c_str(), &error));
  if (committed) {
    CHECK(!std::filesystem::exists(root / "rime_ice.userdb"));
    CHECK(!std::filesystem::exists(root / "wubi86_jidian.userdb"));
    CHECK(HasMarker(root, "famo-userdb.committed"));
    CHECK(HasMarker(root, "famo-userdb.progress"));
  } else {
    CHECK(std::filesystem::exists(root / "rime_ice.userdb" / "CURRENT"));
    CHECK(std::filesystem::exists(root /
                                  "wubi86_jidian.userdb" / "CURRENT"));
    CHECK(HasMarker(root, "famo-userdb.rolledback"));
    CHECK(HasMarker(root, "famo-userdb.progress"));
  }
  recovered.Stop();
  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", L"") == 0);
  return 0;
}

int RunUnknownIdentityDebtTest() {
  const std::filesystem::path unique =
      std::filesystem::temp_directory_path() /
      ("famo-userdb-debt-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()));
  TestDirectory cleanup(unique);
  const std::filesystem::path root = unique / "data";
  const std::filesystem::path lock_root = unique / "local-app-data";
  std::filesystem::create_directories(lock_root);
  CHECK(PrepareRoot(root));
  CHECK(LaunchCrashChild(root, lock_root, L"after-first-move"));
  const std::filesystem::path transaction = FindTransaction(root);
  CHECK(!transaction.empty());
  CHECK(std::filesystem::exists(transaction / "rime_ice.userdb"));
  std::filesystem::rename(transaction / "rime_ice.userdb",
                          transaction / "identity-debt");
  std::filesystem::create_directories(transaction / "rime_ice.userdb");
  std::ofstream(transaction / "rime_ice.userdb" / "CURRENT")
      << "collision";

  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA",
                   lock_root.c_str()) == 0);
  RuntimeService refused;
  TestSink sink;
  refused.SetSnapshotSink(&sink);
  std::string error;
  const std::string root_utf8 = Utf8(root);
  CHECK(!refused.Start(L"FamoTestEngine.dll", root_utf8.c_str(), &error));
  CHECK(refused.readiness() == RuntimeReadiness::Unavailable);
  CHECK(std::filesystem::exists(transaction / "identity-debt" / "CURRENT"));
  CHECK(ReadText(transaction / "rime_ice.userdb" / "CURRENT") ==
        "collision");
  CHECK(std::filesystem::exists(transaction / "famo-userdb.pending"));
  CHECK(!std::filesystem::exists(transaction /
                                 "famo-userdb.rolledback"));
  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", L"") == 0);
  return 0;
}

int RunParentSwapTests() {
  const std::filesystem::path unique =
      std::filesystem::temp_directory_path() /
      ("famo-parent-swap-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()));
  TestDirectory cleanup(unique);
  const std::filesystem::path real_a = unique / "real-a";
  const std::filesystem::path real_b = unique / "real-b";
  const std::filesystem::path profile_a = real_a / "profile";
  const std::filesystem::path profile_b = real_b / "profile";
  const std::filesystem::path alias = unique / "alias";
  const std::filesystem::path lock_root = unique / "local-app-data";
  std::filesystem::create_directories(lock_root);
  CHECK(PrepareRoot(profile_a));
  CHECK(PrepareRoot(profile_b));
  std::ofstream(profile_b / "rime_ice.userdb" / "CURRENT")
      << "outside-rime";
  std::ofstream(profile_b / "wubi86_jidian.userdb" / "CURRENT")
      << "outside-wubi";
  std::ofstream(profile_b / "famo-select-schema.txt", std::ios::binary)
      << "outside-schema-sentinel\n";
  CHECK(CreateJunction(alias, real_a));
  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA",
                   lock_root.c_str()) == 0);

  const std::string aliased_root = Utf8(alias / "profile");
  CHECK(!aliased_root.empty());
  {
    RuntimeService service;
    TestSink sink;
    service.SetSnapshotSink(&sink);
    std::string error;
    CHECK(service.Start(L"FamoTestEngine.dll", aliased_root.c_str(), &error));
    CHECK(service.InitializeControlState() == ControlError::None);

    const std::wstring event_suffix =
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    const std::wstring ready_name =
        L"Local\\Famo.UserdbSwap.Ready." + event_suffix;
    const std::wstring continue_name =
        L"Local\\Famo.UserdbSwap.Continue." + event_suffix;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE proceed =
        CreateEventW(nullptr, TRUE, FALSE, continue_name.c_str());
    CHECK(ready && proceed);
    CHECK(_wputenv_s(L"FAMO_TEST_USERDB_READY_EVENT",
                     ready_name.c_str()) == 0);
    CHECK(_wputenv_s(L"FAMO_TEST_USERDB_CONTINUE_EVENT",
                     continue_name.c_str()) == 0);
    ControlError reset = ControlError::Runtime;
    std::thread worker([&] {
      reset = service.ExecuteControl(Command::ControlResetUserDictionary);
    });
    const bool pinned =
        WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
    const bool swapped = pinned && ReplaceJunction(alias, real_b);
    SetEvent(proceed);
    worker.join();
    CloseHandle(ready);
    CloseHandle(proceed);
    CHECK(_wputenv_s(L"FAMO_TEST_USERDB_READY_EVENT", L"") == 0);
    CHECK(_wputenv_s(L"FAMO_TEST_USERDB_CONTINUE_EVENT", L"") == 0);
    CHECK(swapped);
    CHECK(reset == ControlError::None);
    CHECK(!std::filesystem::exists(profile_a / "rime_ice.userdb"));
    CHECK(!std::filesystem::exists(profile_a / "wubi86_jidian.userdb"));
    CHECK(HasMarker(profile_a, "famo-userdb.committed"));
    CHECK(ReadText(profile_b / "rime_ice.userdb" / "CURRENT") ==
          "outside-rime");
    CHECK(ReadText(profile_b / "wubi86_jidian.userdb" / "CURRENT") ==
          "outside-wubi");
    CHECK(!std::filesystem::exists(profile_b / ".famo-backup"));
    service.Stop();
  }

  CHECK(ReplaceJunction(alias, real_a));
  {
    RuntimeService service;
    TestSink sink;
    service.SetSnapshotSink(&sink);
    std::string error;
    CHECK(service.Start(L"FamoTestEngine.dll", aliased_root.c_str(), &error));
    CHECK(service.InitializeControlState() == ControlError::None);
    const std::wstring event_suffix =
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    const std::wstring ready_name =
        L"Local\\Famo.SchemaSwap.Ready." + event_suffix;
    const std::wstring continue_name =
        L"Local\\Famo.SchemaSwap.Continue." + event_suffix;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE proceed =
        CreateEventW(nullptr, TRUE, FALSE, continue_name.c_str());
    CHECK(ready && proceed);
    CHECK(_wputenv_s(L"FAMO_TEST_SCHEMA_READY_EVENT",
                     ready_name.c_str()) == 0);
    CHECK(_wputenv_s(L"FAMO_TEST_SCHEMA_CONTINUE_EVENT",
                     continue_name.c_str()) == 0);
    ControlError selected = ControlError::Runtime;
    std::thread worker(
        [&] { selected = service.SelectSchemaAndPersist("next"); });
    const bool pinned =
        WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
    const bool swapped = pinned && ReplaceJunction(alias, real_b);
    SetEvent(proceed);
    worker.join();
    CloseHandle(ready);
    CloseHandle(proceed);
    CHECK(_wputenv_s(L"FAMO_TEST_SCHEMA_READY_EVENT", L"") == 0);
    CHECK(_wputenv_s(L"FAMO_TEST_SCHEMA_CONTINUE_EVENT", L"") == 0);
    CHECK(swapped);
    CHECK(selected == ControlError::None);
    CHECK(ReadText(profile_a / "famo-select-schema.txt") == "next");
    CHECK(ReadText(profile_b / "famo-select-schema.txt") ==
          "outside-schema-sentinel\n");
    CHECK(!std::filesystem::exists(
        profile_a / "famo-select-schema.txt.famo-write.tmp"));
    CHECK(!std::filesystem::exists(
        profile_b / "famo-select-schema.txt.famo-write.tmp"));
    service.Stop();
  }
  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", L"") == 0);
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 5 && std::wstring_view(argv[1]) == L"--child")
    return Child(argv[2], argv[3], argv[4]);
  CHECK(argc == 1);
  CHECK(RunPhase(L"after-manifest", false) == 0);
  CHECK(RunPhase(L"after-first-move", false) == 0);
  CHECK(RunPhase(L"before-commit", false) == 0);
  CHECK(RunPhase(L"after-commit", true) == 0);
  CHECK(RunUnknownIdentityDebtTest() == 0);
  CHECK(RunParentSwapTests() == 0);
  std::printf("userdb_transaction_selfcheck: OK\n");
  return 0;
}
