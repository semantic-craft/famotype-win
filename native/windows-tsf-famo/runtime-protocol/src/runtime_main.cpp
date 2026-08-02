#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <shlobj.h>

#include "candidate_window.h"
#include "famo_install_state.h"
#include "famo_runtime_control.h"
#include "famo_runtime_identity.h"
#include "famo_runtime_pipe.h"
#include "famo_status_ui.h"

using namespace famo::runtime;

namespace {

std::wstring ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::wstring DefaultDataRoot() {
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &local))) {
    return {};
  }
  std::wstring result(local);
  CoTaskMemFree(local);
  result += L"\\Famo";
  return result;
}

bool Utf8(std::wstring_view source, std::string *target) {
  if (!target)
    return false;
  const int needed = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
      static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0)
    return false;
  target->resize(static_cast<size_t>(needed));
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
                             static_cast<int>(source.size()), target->data(),
                             needed, nullptr, nullptr) == needed;
}

std::string InstallProjectionSummary() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Famo\\InputMethod", 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return "registry=unavailable";
  }
  wchar_t state[32]{};
  wchar_t directory[32768]{};
  DWORD state_bytes = sizeof(state);
  DWORD directory_bytes = sizeof(directory);
  const LSTATUS state_result =
      RegGetValueW(key, nullptr, L"InstallState", RRF_RT_REG_SZ, nullptr,
                   state, &state_bytes);
  const LSTATUS directory_result =
      RegGetValueW(key, nullptr, L"InstallDir", RRF_RT_REG_SZ, nullptr,
                   directory, &directory_bytes);
  RegCloseKey(key);
  std::string state_utf8;
  std::string directory_utf8;
  if (state_result != ERROR_SUCCESS || directory_result != ERROR_SUCCESS ||
      !Utf8(state, &state_utf8) || !Utf8(directory, &directory_utf8)) {
    return "registry=invalid";
  }
  return "state=" + state_utf8 + " install_dir=" + directory_utf8;
}

void AppendStartupDiagnostic(const std::wstring &data_root,
                             std::string_view stage, unsigned code,
                             std::string_view detail = {}) noexcept {
  try {
    if (data_root.empty())
      return;
    CreateDirectoryW(data_root.c_str(), nullptr);
    const std::wstring log_directory = data_root + L"\\log";
    CreateDirectoryW(log_directory.c_str(), nullptr);
    const std::wstring path =
        log_directory + L"\\famo-runtime-startup.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char prefix[256]{};
    const int prefix_length = std::snprintf(
        prefix, sizeof(prefix),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03u pid=%lu stage=%.*s code=%u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<int>(stage.size()), stage.data(), code);
    if (prefix_length > 0) {
      const size_t safe_length =
          static_cast<size_t>(prefix_length) < sizeof(prefix)
              ? static_cast<size_t>(prefix_length)
              : sizeof(prefix) - 1;
      std::string line(prefix, safe_length);
      if (!detail.empty()) {
        line += " detail=";
        line.append(detail);
      }
      line += "\r\n";
      DWORD written = 0;
      WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written,
                nullptr);
    }
    CloseHandle(file);
  } catch (...) {
  }
}

// #41: bounded exit-code run of the settings companion in headless mode
// (CLI verbs never enter XAML). Returns -1 on spawn failure or timeout; the
// child's own retry loops are bounded, so a timed-out child is left to finish
// rather than killed mid registry write.
int RunSettingsHeadless(const std::wstring &executable,
                        const wchar_t *arguments) {
  std::wstring command = L"\"" + executable + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return -1;
  }
  CloseHandle(process.hThread);
  int exit_code = -1;
  // The add verb is internally bounded (20x500ms retry); leave headroom.
  if (WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0) {
    DWORD code = 0;
    if (GetExitCodeProcess(process.hProcess, &code))
      exit_code = static_cast<int>(code);
  }
  CloseHandle(process.hProcess);
  return exit_code;
}

// RuntimeService holds a single sink, so the candidate window and the tray both
// hang off this. Only Publish and ActivateStyle fan out; PrepareStyle has an
// out-parameter with a single owner, and the prewarm hook is the renderer's.
class SnapshotFanout final : public RuntimeSnapshotSink {
public:
  SnapshotFanout(RuntimeSnapshotSink *renderer, RuntimeSnapshotSink *status)
      : renderer_(renderer), status_(status) {}

  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    renderer_->Publish(snapshot);
    status_->Publish(std::move(snapshot));
  }
  bool PrepareStyle(std::string_view text, bool exists,
                    std::shared_ptr<const void> *presentation) noexcept override {
    return renderer_->PrepareStyle(text, exists, presentation);
  }
  void ActivateStyle(
      std::shared_ptr<const RuntimeStyleState> style) noexcept override {
    renderer_->ActivateStyle(style);
    status_->ActivateStyle(std::move(style));
  }
  void PrepareForRuntimeReady() noexcept override {
    renderer_->PrepareForRuntimeReady();
  }

private:
  RuntimeSnapshotSink *renderer_;
  RuntimeSnapshotSink *status_;
};

} // namespace

int wmain(int argc, wchar_t **argv) {
  std::wstring data_root = DefaultDataRoot();
  std::wstring endpoint_suffix = kDefaultRuntimeEndpointSuffix;
  std::wstring control_endpoint_suffix;
  Command control_command = Command::Hello;
  bool control_mode = false;
  int workers = static_cast<int>(kRuntimeAcceptWorkerCapacity);
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--data-root" && i + 1 < argc) {
      data_root = argv[++i];
    } else if (argument == L"--endpoint-suffix" && i + 1 < argc) {
      endpoint_suffix = argv[++i];
    } else if (argument == L"--workers" && i + 1 < argc) {
      workers = _wtoi(argv[++i]);
    } else if (argument == L"--control" && i + 1 < argc) {
      control_mode = ParseControlCommand(argv[++i], &control_command);
      if (!control_mode) {
        std::fprintf(stderr, "invalid control operation\n");
        return 2;
      }
    } else if (argument == L"--control-endpoint-suffix" && i + 1 < argc) {
      control_endpoint_suffix = argv[++i];
    } else if (argument == L"/q") {
      control_mode = true;
      control_command = Command::ControlShutdown;
    } else {
      std::fprintf(stderr, "invalid arguments\n");
      return 2;
    }
  }
  std::string data_root_utf8;
  PipeEndpoint endpoint;
  PipeEndpoint control_endpoint;
  std::string error;
  if (control_endpoint_suffix.empty()) {
    control_endpoint_suffix = endpoint_suffix == kDefaultRuntimeEndpointSuffix
                                  ? kDefaultControlEndpointSuffix
                                  : endpoint_suffix + L"-control";
  }
  if (control_endpoint_suffix == endpoint_suffix) {
    std::fprintf(stderr, "key and control endpoints must be distinct\n");
    return 2;
  }
  if (!BuildCurrentPipeEndpoint(control_endpoint_suffix, &control_endpoint,
                                &error)) {
    std::fprintf(stderr, "control endpoint setup failed: %s\n", error.c_str());
    return 2;
  }
  if (control_mode) {
    ControlResult result;
    const std::wstring runtime_path = ModuleDirectory() + L"\\FamoRuntime.exe";
    if (!RunControlClient(control_endpoint, runtime_path, control_command,
                          std::chrono::minutes(2), &result, &error)) {
      std::fprintf(stderr, "control transport failed: %s\n", error.c_str());
      return 3;
    }
    std::printf("operation=%llu state=%u error=%u retryable=%u readiness=%u generation=%llu\n",
                static_cast<unsigned long long>(result.operation_id),
                static_cast<unsigned>(result.state),
                static_cast<unsigned>(result.error), result.retryable ? 1u : 0u,
                static_cast<unsigned>(result.readiness),
                static_cast<unsigned long long>(result.engine_generation));
    return result.state == ControlState::Succeeded
               ? 0
               : 10 + static_cast<int>(result.error);
  }
  if (endpoint_suffix == kDefaultRuntimeEndpointSuffix &&
      !ProductionInstallAllowed(ModuleDirectory(), true)) {
    AppendStartupDiagnostic(data_root, "install-state", 3,
                            InstallProjectionSummary());
    std::fprintf(stderr, "runtime install state is not active\n");
    return 3;
  }
  const bool root_ready = CreateDirectoryW(data_root.c_str(), nullptr) != FALSE ||
                          GetLastError() == ERROR_ALREADY_EXISTS;
  // The primary accept pool must be able to service every logical client slot;
  // one extra acceptor returns protocol-level Unavailable at capacity. The UI
  // lane has its own equally-sized pool below.
  if (data_root.empty() || endpoint_suffix.empty() ||
      workers != static_cast<int>(kRuntimeAcceptWorkerCapacity) || !root_ready ||
      !Utf8(data_root, &data_root_utf8) ||
      !BuildCurrentPipeEndpoint(endpoint_suffix, &endpoint, &error)) {
    std::fprintf(stderr, "runtime setup failed: %s\n", error.c_str());
    return 2;
  }
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);

  const std::wstring singleton_name =
      L"Local\\" + std::wstring(kRuntimeSingletonPrefix) + L"." +
      std::to_wstring(endpoint.session_id) +
      L"." + endpoint_suffix;
  HANDLE singleton = CreateMutexW(nullptr, TRUE, singleton_name.c_str());
  if (!singleton) {
    AppendStartupDiagnostic(data_root, "singleton", 3);
    std::fprintf(stderr, "runtime singleton setup failed\n");
    return 3;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(singleton);
    return 0;
  }

  // #41: bounded TIP self-heal. A clean install starts this runtime while its
  // projection is still Activating, so a one-shot Ready check can permanently
  // miss the transition. Wait only while this exact target remains Activating;
  // PendingReboot, RolledBack, uninstall, or a different target stop the task.
  // Once Ready, delegate the full probe/add/two-stable-readback loop to the
  // settings companion instead of copying a third implementation. Windows can
  // still rebuild the user input-source list as Setup exits, so repeat the
  // delegated check after a bounded Ready-only settling window. Detached
  // thread: startup and typing never block on either check.
  std::thread([data_root] {
    constexpr int kTipSelfHealReadyAttempts = 60;
    constexpr DWORD kTipSelfHealReadyDelayMs = 1000;
    bool ready = false;
    for (int attempt = 0; attempt < kTipSelfHealReadyAttempts; ++attempt) {
      if (!ProductionInstallAllowed(ModuleDirectory(), true))
        return;
      if (ProductionInstallAllowed(ModuleDirectory(), false)) {
        ready = true;
        break;
      }
      Sleep(kTipSelfHealReadyDelayMs);
    }
    if (!ready) {
      AppendStartupDiagnostic(data_root, "tip-selfheal", 3,
                              "ready transition timed out");
      return;
    }
    const std::wstring settings =
        ModuleDirectory() + L"\\settings\\FamoSettings.exe";
    const int result = RunSettingsHeadless(settings, L"--tip-self-heal");
    if (result != 0)
      AppendStartupDiagnostic(data_root, "tip-selfheal", 3,
                              "initial exit=" + std::to_string(result));
    if (result < 0)
      return; // do not overlap a companion that may have hit our wait bound

    constexpr int kTipSelfHealPostReadyAttempts = 10;
    constexpr DWORD kTipSelfHealPostReadyDelayMs = 1000;
    for (int attempt = 0; attempt < kTipSelfHealPostReadyAttempts; ++attempt) {
      Sleep(kTipSelfHealPostReadyDelayMs);
      if (!ProductionInstallAllowed(ModuleDirectory(), false))
        return;
    }
    const int settled_result =
        RunSettingsHeadless(settings, L"--tip-self-heal");
    if (settled_result != 0)
      AppendStartupDiagnostic(data_root, "tip-selfheal", 3,
                              "settled exit=" +
                                  std::to_string(settled_result));
  }).detach();

  CandidateWindow candidate_window;
  if (!candidate_window.Start()) {
    AppendStartupDiagnostic(data_root, "candidate-window", 3);
    std::fprintf(stderr, "candidate window thread setup failed\n");
    return 3;
  }
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi status_ui(&service, &running, data_root);
  SnapshotFanout snapshots(&candidate_window, &status_ui);
  service.SetSnapshotSink(&snapshots);
  const std::wstring engine = ModuleDirectory() + L"\\FamoRimeEngine.dll";
  if (!service.Start(engine.c_str(), data_root_utf8.c_str(), &error)) {
    AppendStartupDiagnostic(data_root, "engine", 4, error);
    std::fprintf(stderr, "engine setup failed: %s\n", error.c_str());
    service.SetSnapshotSink(nullptr);
    candidate_window.Stop();
    return 4;
  }

  // Load persisted control state before accepting keys. Malformed overlays
  // keep readiness Unavailable while the control endpoint remains available
  // for a visible, explicit retry after the file is corrected.
  const ControlError startup_control = service.InitializeControlState();
  if (startup_control != ControlError::None)
    std::fprintf(stderr, "persisted control state invalid: category=%u\n",
                 static_cast<unsigned>(startup_control));

  RuntimeControlService control_service(&service, &running);
  if (!control_service.Start()) {
    AppendStartupDiagnostic(data_root, "control-pipe", 5);
    service.SetSnapshotSink(nullptr);
    service.Stop();
    candidate_window.Stop();
    return 5;
  }
  // A tray icon that fails to register must not take the input method down with
  // it; typing keeps working, the menu is simply unreachable.
  if (!status_ui.Start())
    std::fprintf(stderr, "tray icon setup failed\n");
  else if (!status_ui.keyboard_hook_ready())
    std::fprintf(stderr, "global shortcut hook degraded: error=%u\n",
                 status_ui.keyboard_hook_error());

  std::vector<std::thread> servers;
  PipeServerStop key_server_stop;
  PipeServerStop ui_server_stop;
  PipeServerStop control_server_stop;
  bool worker_pool_ready = true;
  try {
    servers.reserve(static_cast<size_t>((workers * 2) + 2));
  } catch (...) {
    worker_pool_ready = false;
    running.store(false);
  }
  for (int worker = 0; worker < workers && worker_pool_ready; ++worker) {
    try {
      servers.emplace_back([&] {
        try {
          while (running.load()) {
            RuntimePipeServer server;
            std::string serve_error;
            if (!server.ServeOnce(endpoint, &service, ServerFault::None,
                                  std::chrono::milliseconds(250), &serve_error,
                                  0, &key_server_stop)) {
              Sleep(10);
            }
          }
        } catch (...) {
          running.store(false);
        }
      });
    } catch (...) {
      worker_pool_ready = false;
      running.store(false);
      break;
    }
  }
  // UI updates have their own accept pool so best-effort candidate-window
  // traffic can never consume a primary key connection slot.
  for (int worker = 0; worker < workers && worker_pool_ready; ++worker) {
    try {
      servers.emplace_back([&] {
        try {
          while (running.load()) {
            RuntimePipeServer server;
            std::string serve_error;
            if (!server.ServeOnce(ui_endpoint, &service, ServerFault::None,
                                  std::chrono::milliseconds(250), &serve_error,
                                  0, &ui_server_stop, true)) {
              Sleep(10);
            }
          }
        } catch (...) {
          running.store(false);
        }
      });
    } catch (...) {
      worker_pool_ready = false;
      running.store(false);
      break;
    }
  }
  for (int worker = 0; worker < 2 && worker_pool_ready; ++worker) {
    try {
      servers.emplace_back([&] {
        try {
          while (running.load()) {
            ControlPipeServer server;
            std::string serve_error;
            if (!server.ServeOnce(control_endpoint, &control_service,
                                  std::chrono::milliseconds(250), &serve_error,
                                  &control_server_stop))
              Sleep(10);
          }
        } catch (...) {
          running.store(false);
        }
      });
    } catch (...) {
      worker_pool_ready = false;
      running.store(false);
      break;
    }
  }
  if (!worker_pool_ready)
    std::fprintf(stderr, "runtime worker pool setup failed\n");
  while (running.load())
    Sleep(10);
  key_server_stop.Stop();
  ui_server_stop.Stop();
  // Let the short-lived shutdown client read its terminal status before its
  // own control connection is cancelled.
  Sleep(250);
  control_server_stop.Stop();
  for (std::thread &server : servers)
    server.join();
  control_service.Stop();
  // Ahead of service teardown: a deploy launched from the menu still holds the
  // service, and Stop() joins that future.
  status_ui.Stop();
  service.SetSnapshotSink(nullptr);
  service.Stop();
  candidate_window.Stop();
  ReleaseMutex(singleton);
  CloseHandle(singleton);
  return 0;
}
