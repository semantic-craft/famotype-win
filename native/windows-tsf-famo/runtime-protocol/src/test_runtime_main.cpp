#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "famo_runtime_pipe.h"
#include "famo_runtime_protocol.h"

using namespace famo::runtime;

namespace {

constexpr wchar_t kTestRuntimePreviewSourceClass[] =
    L"FamoTestRuntimePreviewSource";

LRESULT CALLBACK PreviewSourceProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::string NarrowAscii(std::wstring_view value) {
  std::string narrowed;
  narrowed.reserve(value.size());
  for (const wchar_t ch : value) {
    if (ch < 0 || ch > 0x7f)
      return {};
    narrowed.push_back(static_cast<char>(ch));
  }
  return narrowed;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  std::wstring suffix;
  std::string fault_name = "none";
  int connections = 1;
  int fault_after = 0;
  bool parallel = false;
  bool inline_preedit = true;
  int preview_rows = 0;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--endpoint-suffix" && i + 1 < argc) {
      suffix = argv[++i];
    } else if (argument == L"--fault" && i + 1 < argc) {
      fault_name = NarrowAscii(argv[++i]);
    } else if (argument == L"--connections" && i + 1 < argc) {
      connections = _wtoi(argv[++i]);
    } else if (argument == L"--fault-after" && i + 1 < argc) {
      fault_after = _wtoi(argv[++i]);
    } else if (argument == L"--parallel") {
      parallel = true;
    } else if (argument == L"--inline-preedit" && i + 1 < argc) {
      const std::wstring_view value(argv[++i]);
      if (value != L"true" && value != L"false") {
        std::fprintf(stderr, "invalid inline-preedit value\n");
        return 2;
      }
      inline_preedit = value == L"true";
    } else if (argument == L"--preview-rows" && i + 1 < argc) {
      preview_rows = _wtoi(argv[++i]);
    } else {
      std::fprintf(stderr, "invalid arguments\n");
      return 2;
    }
  }
  ServerFault fault;
  PipeEndpoint endpoint;
  std::string error;
  const int max_connections =
      parallel ? static_cast<int>(kRuntimeClientCapacity * 2) : 4;
  if (connections < 1 || connections > max_connections || fault_after < 0 ||
      fault_after > 100 ||
      preview_rows < 0 || preview_rows > 2 ||
      !ParseServerFault(fault_name, &fault) ||
      !BuildCurrentPipeEndpoint(suffix, &endpoint, &error)) {
    std::fprintf(stderr, "runtime setup failed: %s\n", error.c_str());
    return 2;
  }
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  WNDCLASSW preview_source_class{};
  preview_source_class.lpfnWndProc = PreviewSourceProc;
  preview_source_class.hInstance = GetModuleHandleW(nullptr);
  preview_source_class.lpszClassName = kTestRuntimePreviewSourceClass;
  if (!RegisterClassW(&preview_source_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    std::fprintf(stderr, "preview source class registration failed\n");
    return 2;
  }
  const std::wstring preview_source_title =
      std::to_wstring(GetCurrentProcessId());
  HWND preview_source = CreateWindowExW(
      0, kTestRuntimePreviewSourceClass, preview_source_title.c_str(), 0, 0, 0,
      0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!preview_source) {
    std::fprintf(stderr, "preview source window creation failed\n");
    return 2;
  }

  if (preview_rows > 0)
    _putenv_s("FAMO_TEST_MULTIPAGE", "1");
  const std::wstring engine = ModuleDirectory() + L"\\FamoTestEngine.dll";
  RuntimeService service;
  if (!service.Start(engine.c_str(), "", &error)) {
    std::fprintf(stderr, "engine setup failed: %s\n", error.c_str());
    return 3;
  }
  uint32_t behavior_flags = inline_preedit ? kHostInlinePreedit : 0;
  if (preview_rows > 0)
    behavior_flags |= kHostPreviewPages;
  if (preview_rows == 2)
    behavior_flags |= kHostPreviewRowsTwo;
  if (service.InitializeControlState(behavior_flags) != ControlError::None) {
    std::fprintf(stderr, "runtime control state setup failed\n");
    return 3;
  }
  if (parallel) {
    std::vector<std::atomic<bool>> served(static_cast<size_t>(connections));
    std::vector<std::string> errors(static_cast<size_t>(connections));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(connections));
    for (int connection = 0; connection < connections; ++connection) {
      workers.emplace_back([&, connection] {
        RuntimePipeServer server;
        const PipeEndpoint &active_endpoint =
            (connection % 2) == 0 ? endpoint : ui_endpoint;
        served[connection].store(server.ServeOnce(
            active_endpoint, &service, fault, std::chrono::seconds(60),
            &errors[connection], static_cast<uint32_t>(fault_after), nullptr,
            (connection % 2) != 0));
      });
    }
    for (std::thread &worker : workers)
      worker.join();
    for (int connection = 0; connection < connections; ++connection) {
      if (!served[connection].load()) {
        std::fprintf(stderr, "runtime serve failed: %s\n",
                     errors[connection].c_str());
        return 4;
      }
    }
    DestroyWindow(preview_source);
    return 0;
  }
  RuntimePipeServer server;
  for (int connection = 0; connection < connections; ++connection) {
    const ServerFault active_fault =
        connection == 0 ? fault : ServerFault::None;
    if (!server.ServeOnce(endpoint, &service, active_fault,
                          std::chrono::seconds(10), &error,
                          static_cast<uint32_t>(fault_after))) {
      std::fprintf(stderr, "runtime serve failed: %s\n", error.c_str());
      return 4;
    }
  }
  DestroyWindow(preview_source);
  return 0;
}
