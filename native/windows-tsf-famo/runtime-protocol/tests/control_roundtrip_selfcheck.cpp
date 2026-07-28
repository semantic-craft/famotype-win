#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include <windows.h>

#include "famo_runtime_control.h"
#include "famo_runtime_pipe.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,       \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  return path;
}

std::wstring ModuleDirectory() {
  const std::wstring path = ModulePath();
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

bool RunWrongExecutableClient(std::wstring_view suffix,
                              std::wstring_view expected_server) {
  const std::wstring intruder =
      ModuleDirectory() + L"\\pipe_roundtrip_selfcheck.exe";
  std::wstring command =
      L"\"" + intruder + L"\" --control-intruder " +
      std::wstring(suffix) + L" \"" + std::wstring(expected_server) + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(intruder.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
  DWORD exit_code = STILL_ACTIVE;
  GetExitCodeProcess(process.hProcess, &exit_code);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 9);
    WaitForSingleObject(process.hProcess, 1000);
  }
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0 && exit_code == 0;
}

int main() {
  std::string error;
  RuntimeService runtime;
  CHECK(runtime.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(runtime.InitializeControlState() == ControlError::None);
  std::atomic<bool> running{true};
  RuntimeControlService control(&runtime, &running);
  CHECK(control.Start());

  const std::wstring unique =
      L"control-selfcheck-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint control_endpoint, key_endpoint;
  CHECK(BuildCurrentPipeEndpoint(unique, &control_endpoint, &error));
  CHECK(BuildCurrentPipeEndpoint(unique + L"-key", &key_endpoint, &error));
  CHECK(control_endpoint.name != key_endpoint.name);

  bool rejected_server_ok = true;
  std::thread rejected_server([&] {
    ControlPipeServer pipe_server;
    std::string server_error;
    rejected_server_ok =
        pipe_server.ServeOnce(control_endpoint, &control,
                              std::chrono::seconds(5), &server_error);
  });
  CHECK(RunWrongExecutableClient(unique, ModulePath()));
  rejected_server.join();
  CHECK(!rejected_server_ok);
  CHECK(runtime.engine_generation() == 1);

  bool server_ok = false;
  std::thread server([&] {
    ControlPipeServer pipe_server;
    std::string server_error;
    server_ok = pipe_server.ServeOnce(control_endpoint, &control,
                                      std::chrono::seconds(5), &server_error);
  });
  ControlResult result;
  CHECK(RunControlClient(control_endpoint, ModulePath(), Command::ControlDeploy,
                         std::chrono::seconds(5), &result, &error));
  CHECK(result.state == ControlState::Succeeded);
  CHECK(result.engine_generation == 2);
  server.join();
  CHECK(server_ok);

  const PipeClientIdentity owner{GetCurrentProcessId(), 12345};
  for (uint64_t index = 0; index < 64; ++index) {
    Frame hello;
    hello.command = Command::Hello;
    hello.correlation = {1000 + index, 1, 1, 0, 0, 0};
    CHECK(control.Dispatch(hello, owner).status == Status::Ok);
  }
  Frame overflow;
  overflow.command = Command::Hello;
  overflow.correlation = {2000, 1, 1, 0, 0, 0};
  CHECK(control.Dispatch(overflow, owner).status == Status::Unavailable);
  for (uint64_t index = 0; index < 64; ++index)
    control.InvalidateConnection(1000 + index, 1, 1, owner);

  control.Stop();
  runtime.Stop();
  std::printf("control_roundtrip_selfcheck: OK\n");
  return 0;
}
