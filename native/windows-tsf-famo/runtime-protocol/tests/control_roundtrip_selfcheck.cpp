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

  control.Stop();
  runtime.Stop();
  std::printf("control_roundtrip_selfcheck: OK\n");
  return 0;
}
