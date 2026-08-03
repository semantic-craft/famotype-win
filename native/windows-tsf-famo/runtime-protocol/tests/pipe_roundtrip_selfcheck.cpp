#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <sddl.h>

#include "famo_pipe_security.h"
#include "famo_runtime_control.h"
#include "famo_runtime_pipe.h"
#include "../src/pipe_io.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace {

bool PipeSecurityAllowsRestrictedClients() {
  PipeEndpoint endpoint{L"\\\\.\\pipe\\Famo.Runtime.v2.test.1.default",
                        L"S-1-5-21-111-222-333-1001", 1};
  SECURITY_ATTRIBUTES attributes{};
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  CHECK(BuildPipeSecurity(endpoint, &attributes, &descriptor, nullptr));
  LPWSTR sddl = nullptr;
  CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(
      descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &sddl,
      nullptr));
  const std::wstring security(sddl);
  LocalFree(sddl);
  LocalFree(descriptor);
  CHECK(security.find(L"(A;;GA;;;SY)") != std::wstring::npos);
  CHECK(security.find(L"(A;;GA;;;S-1-5-21-111-222-333-1001)") !=
        std::wstring::npos);
  CHECK(security.find(L"(A;;GWGR;;;AC)") != std::wstring::npos);
  CHECK(security.find(L"(A;;GWGR;;;S-1-15-2-2)") != std::wstring::npos);
  CHECK(security.find(L";;;WD)") == std::wstring::npos);
  CHECK(security.find(L";;;AN)") == std::wstring::npos);
  return true;
}

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  return path;
}

std::wstring ModuleDirectory() {
  std::wstring path = ModulePath();
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::wstring RuntimePath() {
  return ModuleDirectory() + L"\\FamoTestRuntime.exe";
}

std::wstring EnginePath() {
  return ModuleDirectory() + L"\\FamoTestEngine.dll";
}

bool SpawnRuntime(std::wstring_view suffix, std::wstring_view fault,
                  PROCESS_INFORMATION *process, int connections = 2,
                  bool parallel = true, int preview_rows = 0) {
  std::wstring command = L"\"" + RuntimePath() + L"\" --endpoint-suffix " +
                         std::wstring(suffix) + L" --fault " +
                         std::wstring(fault) + L" --connections " +
                         std::to_wstring(connections);
  if (parallel)
    command += L" --parallel";
  if (preview_rows > 0)
    command += L" --preview-rows " + std::to_wstring(preview_rows);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, ModuleDirectory().c_str(),
                        &startup, process) != FALSE;
}

bool FinishRuntime(PROCESS_INFORMATION *process) {
  CloseHandle(process->hThread);
  const DWORD wait = WaitForSingleObject(process->hProcess, 3000);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process->hProcess, 9);
    WaitForSingleObject(process->hProcess, 1000);
  }
  DWORD exit_code = 0;
  GetExitCodeProcess(process->hProcess, &exit_code);
  CloseHandle(process->hProcess);
  return wait == WAIT_OBJECT_0 && exit_code == 0;
}

bool RunWrongOwnerChild(std::wstring_view suffix,
                        const Correlation &identity) {
  std::wstring command =
      L"\"" + ModulePath() + L"\" --wrong-owner " + std::wstring(suffix) +
      L" " + std::to_wstring(identity.client_id) + L" " +
      std::to_wstring(identity.activation_generation) + L" " +
      std::to_wstring(identity.connection_generation);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, ModuleDirectory().c_str(),
                      &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 3000);
  DWORD exit_code = STILL_ACTIVE;
  GetExitCodeProcess(process.hProcess, &exit_code);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 9);
    WaitForSingleObject(process.hProcess, 1000);
  }
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0 && exit_code == 0;
}

Frame Request(Command command, uint64_t generation, uint64_t sequence) {
  Frame frame;
  frame.command = command;
  frame.correlation = {101, 102, generation, 103, 104, sequence};
  return frame;
}

Frame Hello(uint64_t generation) {
  Frame frame = Request(Command::Hello, generation, 0);
  frame.correlation.session_id = 0;
  frame.correlation.session_generation = 0;
  return frame;
}

CallResult DeliveredCall(PipeRuntimePort &port, Frame &&request) {
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + kHardCallDeadline;
  const DeliveryReference reference{request.command, request.correlation};
  CallResult prepared = port.Prepare(std::move(request), deadline);
  if (prepared.status != Status::Prepared)
    return prepared;
  DeliveryResult delivered = port.ExecutePrepared(reference, deadline);
  CallResult result;
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  if (delivered.status != Status::Ok) {
    result.status = delivered.status;
    return result;
  }
  result.reply = std::move(delivered.final_reply);
  result.status = result.reply.status;
  const CallResult acknowledged =
      port.Ack(reference, std::chrono::steady_clock::now() +
                              kHardCallDeadline);
  if (acknowledged.status != Status::Ok)
    result.status = acknowledged.status;
  return result;
}

class BlockingSink final : public RuntimeSnapshotSink {
public:
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    (void)snapshot;
    std::unique_lock lock(mutex_);
    if (!armed_)
      return;
    entered_value_ = true;
    entered_.notify_all();
    released_.wait(lock, [&] { return !armed_; });
  }

  void Arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
    entered_value_ = false;
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return entered_.wait_for(lock, timeout, [&] { return entered_value_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      armed_ = false;
    }
    released_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable entered_;
  std::condition_variable released_;
  bool armed_ = false;
  bool entered_value_ = false;
};

class RecordingSink final : public RuntimeSnapshotSink {
public:
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    {
      std::lock_guard lock(mutex_);
      latest_ = std::move(snapshot);
    }
    changed_.notify_all();
  }

  bool WaitForUiSequence(uint64_t sequence,
                         std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] {
      return latest_ && latest_->ui_sequence >= sequence;
    });
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<const RuntimeSnapshot> latest_;
};

bool NormalRoundtrip() {
  const std::wstring suffix =
      L"roundtrip-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  CHECK(endpoint.name.find(endpoint.user_sid) != std::wstring::npos);
  CHECK(endpoint.name.find(std::to_wstring(endpoint.session_id)) !=
        std::wstring::npos);
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process));

  PipeRuntimePort port;
  constexpr uint64_t generation = 200;
  const bool connected =
      port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                   std::chrono::seconds(2), &error);
  if (!connected) {
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    std::fprintf(stderr, "normal connect failed: %s (runtime=%lu)\n",
                 error.c_str(), static_cast<unsigned long>(exit_code));
  }
  CHECK(connected);

  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kHardCallDeadline).status == Status::Ok);

  // An inactive editor can keep one TSF context alive for arbitrarily long.
  // The out-of-process runtime must not treat that normal idle time as an I/O
  // failure or the first key after every idle period will fail open.
  Sleep(10100);

  // A completed call must release the worker slot before it wakes the caller.
  // Immediate back-to-back keys must never observe a stale in-flight marker.
  for (uint64_t sequence = 2; sequence < 102; ++sequence) {
    Frame unhandled = Request(Command::ProcessKey, generation, sequence);
    CHECK(EncodeKeyEvent({0x7b, 0, 0, 1, sequence}, &unhandled.payload));
    const CallResult immediate =
        DeliveredCall(port, std::move(unhandled));
    CHECK(immediate.status == Status::Ok);
  }

  Frame key = Request(Command::ProcessKey, generation, 102);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1}, &key.payload));
  CallResult result = DeliveredCall(port, std::move(key));
  CHECK(result.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");

  key = Request(Command::ProcessKey, generation, 103);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2}, &key.payload));
  result = DeliveredCall(port, std::move(key));
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.preedit == "ni" && !composition.candidates.empty());

  Frame select = Request(Command::SelectCandidate, generation, 104);
  CHECK(EncodeCandidateIndex(0, &select.payload));
  result = DeliveredCall(port, std::move(select));
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.commit == "\xe4\xbd\xa0");

  Frame unhandled = Request(Command::ProcessKey, generation, 105);
  CHECK(EncodeKeyEvent({0x7b, 0, 0, 1, 3}, &unhandled.payload));
  result = DeliveredCall(port, std::move(unhandled));
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(!composition.handled);

  Frame close = Request(Command::CloseSession, generation, 106);
  CHECK(port.Call(std::move(close), kHardCallDeadline).status == Status::Ok);
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool ExtendedHelloRoundtrip() {
  const std::wstring suffix =
      L"extended-hello-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process));

  PipeRuntimePort port(1);
  constexpr uint64_t generation = 199;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kSessionOpenDeadline).status == Status::Ok);
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool AbsolutePreviewSelectionRoundtrip() {
  const std::wstring suffix =
      L"absolute-preview-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  const bool spawned = SpawnRuntime(suffix, L"none", &process, 2, true, 2);
  CHECK(spawned);

  PipeRuntimePort port;
  constexpr uint64_t generation = 201;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kSessionOpenDeadline).status == Status::Ok);

  Composition composition;
  for (uint64_t sequence = 2; sequence <= 3; ++sequence) {
    Frame key = Request(Command::ProcessKey, generation, sequence);
    const uint32_t letter = sequence == 2 ? 'N' : 'I';
    CHECK(EncodeKeyEvent({letter, 0, 0, 1, sequence}, &key.payload));
    CallResult result = DeliveredCall(port, std::move(key));
    CHECK(result.status == Status::Ok);
    CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  }
  CHECK(composition.page_size == 1);
  CHECK(composition.is_last_page == 0);

  Frame stale = Request(Command::SelectCandidateAbsolute, generation, 4);
  CHECK(EncodeAbsoluteCandidateSelection(1, 2, &stale.payload));
  const CallResult stale_result = DeliveredCall(port, std::move(stale));
  CHECK(stale_result.status == Status::StaleRequest);
  Frame out_of_range =
      Request(Command::SelectCandidateAbsolute, generation, 5);
  CHECK(EncodeAbsoluteCandidateSelection(3, 3, &out_of_range.payload));
  CHECK(DeliveredCall(port, std::move(out_of_range)).status ==
        Status::InvalidFrame);

  Frame page_two =
      Request(Command::SelectCandidateAbsolute, generation, 6);
  CHECK(EncodeAbsoluteCandidateSelection(1, 3, &page_two.payload));
  CallResult selected = DeliveredCall(port, std::move(page_two));
  CHECK(selected.status == Status::Ok);
  CHECK(DecodeComposition(selected.reply.payload, &composition, &error));
  CHECK(composition.commit == "\xe5\xb0\xbc");

  for (uint64_t sequence = 7; sequence <= 8; ++sequence) {
    Frame key = Request(Command::ProcessKey, generation, sequence);
    const uint32_t letter = sequence == 7 ? 'N' : 'I';
    CHECK(EncodeKeyEvent({letter, 0, 0, 1, sequence}, &key.payload));
    selected = DeliveredCall(port, std::move(key));
    CHECK(selected.status == Status::Ok);
    CHECK(DecodeComposition(selected.reply.payload, &composition, &error));
  }
  Frame page_three =
      Request(Command::SelectCandidateAbsolute, generation, 9);
  CHECK(EncodeAbsoluteCandidateSelection(2, 8, &page_three.payload));
  selected = DeliveredCall(port, std::move(page_three));
  CHECK(selected.status == Status::Ok);
  CHECK(DecodeComposition(selected.reply.payload, &composition, &error));
  CHECK(composition.commit == "\xe6\xb3\xa5");

  Frame close = Request(Command::CloseSession, generation, 10);
  CHECK(port.Call(std::move(close), kHardCallDeadline).status == Status::Ok);
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool WrongPeerRejected() {
  const std::wstring suffix =
      L"wrong-peer-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process, 1, false));
  PipeRuntimePort port;
  CHECK(!port.Connect(endpoint, RuntimePath() + L".wrong",
                      Hello(300).correlation, std::chrono::seconds(2), &error));
  CHECK(FinishRuntime(&process));
  return true;
}

bool ConnectFailureIsOffHotPath() {
  PipeEndpoint endpoint;
  std::string error;
  const std::wstring suffix =
      L"absent-" + std::to_wstring(GetCurrentProcessId());
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PipeRuntimePort port;
  const auto started = std::chrono::steady_clock::now();
  CHECK(!port.Connect(endpoint, RuntimePath(), Hello(400).correlation,
                      kHardCallDeadline, &error));
  const auto connect_elapsed = std::chrono::steady_clock::now() - started;
  CHECK(connect_elapsed <= kHardCallDeadline + std::chrono::milliseconds(15));
  const auto call_started = std::chrono::steady_clock::now();
  CHECK(port.Call(Hello(400), kHardCallDeadline).status == Status::Unavailable);
  CHECK(std::chrono::steady_clock::now() - call_started <
        std::chrono::milliseconds(10));
  return true;
}

bool OptionalUiPipeDoesNotDelayPrimaryReadiness() {
  const std::wstring suffix =
      L"optional-ui-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  RuntimeService service;
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  bool served = false;
  std::string server_error;
  std::thread server([&] {
    RuntimePipeServer pipe_server;
    served = pipe_server.ServeOnce(endpoint, &service, ServerFault::None,
                                   std::chrono::seconds(2), &server_error);
  });
  Sleep(25);

  PipeRuntimePort port;
  const auto started = std::chrono::steady_clock::now();
  const bool connected =
      port.Connect(endpoint, ModulePath(), Hello(425).correlation,
                   std::chrono::seconds(2), &error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  port.Stop();
  server.join();
  service.Stop();

  CHECK(connected);
  CHECK(elapsed < std::chrono::milliseconds(100));
  CHECK(served);
  return true;
}

bool OptionalUiPipeRecoversWhenItAppears() {
  const std::wstring suffix =
      L"optional-ui-recovery-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  RuntimeService service;
  RecordingSink sink;
  service.SetSnapshotSink(&sink);
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  bool primary_served = false;
  std::string primary_error;
  std::thread primary([&] {
    RuntimePipeServer server;
    primary_served = server.ServeOnce(
        endpoint, &service, ServerFault::None, std::chrono::seconds(2),
        &primary_error);
  });
  Sleep(25);

  PipeRuntimePort port;
  constexpr uint64_t generation = 426;
  const bool connected =
      port.Connect(endpoint, ModulePath(), Hello(generation).correlation,
                   std::chrono::seconds(2), &error);
  bool opened = false;
  if (connected) {
    Frame open = Request(Command::OpenSession, generation, 1);
    opened = EncodeOpenSession("test", &open.payload, &error) &&
             port.Call(std::move(open), kHardCallDeadline).status == Status::Ok;
  }

  bool ui_served = false;
  std::string ui_error;
  std::thread ui([&] {
    RuntimePipeServer server;
    ui_served = server.ServeOnce(
        ui_endpoint, &service, ServerFault::None, std::chrono::seconds(2),
        &ui_error, 0, nullptr, true);
  });
  Sleep(25);

  bool posted = false;
  bool recovered = false;
  if (opened) {
    Frame update = Request(Command::UpdateUiState, generation, 2);
    posted =
        EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true, {1, 2}},
                      &update.payload, &error);
    if (posted) {
      port.Post(std::move(update));
      recovered =
          sink.WaitForUiSequence(2, std::chrono::seconds(1));
    }
  }

  port.Stop();
  primary.join();
  ui.join();
  service.SetSnapshotSink(nullptr);
  service.Stop();

  if (!primary_served)
    std::fprintf(stderr, "primary server failed: %s\n",
                 primary_error.c_str());
  if (!ui_served)
    std::fprintf(stderr, "UI server failed: %s\n", ui_error.c_str());
  CHECK(connected);
  CHECK(opened);
  CHECK(posted);
  CHECK(recovered);
  CHECK(primary_served);
  CHECK(ui_served);
  return true;
}

bool UiEndpointRejectsBusinessCommands() {
  const std::wstring suffix =
      L"ui-command-isolation-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  RuntimeService service;
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  bool served = false;
  std::string server_error;
  std::thread server([&] {
    RuntimePipeServer pipe_server;
    served = pipe_server.ServeOnce(
        ui_endpoint, &service, ServerFault::None, std::chrono::seconds(2),
        &server_error, 0, nullptr, true);
  });

  HANDLE pipe = INVALID_HANDLE_VALUE;
  const auto connect_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (pipe == INVALID_HANDLE_VALUE &&
         std::chrono::steady_clock::now() < connect_deadline) {
    pipe = CreateFileW(ui_endpoint.name.c_str(), GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                       nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
      Sleep(1);
  }
  bool isolated = false;
  if (pipe != INVALID_HANDLE_VALUE) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Frame hello = Hello(445);
    Frame reply;
    const bool greeted =
        pipe_io::WriteFrame(pipe, hello, deadline, &error) ==
            pipe_io::Result::Ok &&
        pipe_io::ReadFrame(pipe, &reply, deadline, &error) ==
            pipe_io::Result::Ok &&
        reply.command == Command::Hello && reply.status == Status::Ok;
    Frame open = Request(Command::OpenSession, 445, 1);
    const bool encoded = EncodeOpenSession("test", &open.payload, &error);
    Frame rejected;
    isolated =
        greeted && encoded &&
        pipe_io::WriteFrame(pipe, open, deadline, &error) ==
            pipe_io::Result::Ok &&
        pipe_io::ReadFrame(pipe, &rejected, deadline, &error) ==
            pipe_io::Result::Ok &&
        rejected.command == Command::OpenSession &&
        rejected.status == Status::InvalidFrame &&
        rejected.correlation == open.correlation;
    CloseHandle(pipe);
  }
  server.join();
  service.Stop();
  CHECK(served);
  CHECK(isolated);
  return true;
}

bool ConcurrentClients() {
  const std::wstring suffix =
      L"concurrent-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  RuntimeService service;
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  std::array<bool, 3> served{};
  std::array<std::string, 3> server_errors;
  std::array<std::thread, 3> servers;
  for (size_t index = 0; index < servers.size(); ++index) {
    servers[index] = std::thread([&, index] {
      RuntimePipeServer server;
      const PipeEndpoint &active_endpoint =
          index < 2 ? endpoint : ui_endpoint;
      served[index] = server.ServeOnce(
          active_endpoint, &service, ServerFault::None,
          std::chrono::seconds(2), &server_errors[index], 0, nullptr,
          index >= 2);
    });
  }
  Sleep(50);

  PipeRuntimePort first;
  PipeRuntimePort second;
  Frame first_hello = Hello(450);
  first_hello.correlation.client_id = 201;
  Frame second_hello = Hello(460);
  second_hello.correlation.client_id = 202;
  const bool first_connected =
      first.Connect(endpoint, ModulePath(), first_hello.correlation,
                    std::chrono::seconds(2), &error);
  const auto second_started = std::chrono::steady_clock::now();
  const bool second_connected =
      second.Connect(endpoint, ModulePath(), second_hello.correlation,
                     std::chrono::seconds(2), &error);
  const auto second_elapsed = std::chrono::steady_clock::now() - second_started;
  first.Stop();
  second.Stop();
  for (auto &server : servers)
    server.join();
  service.Stop();

  CHECK(first_connected);
  CHECK(second_connected);
  CHECK(second_elapsed < std::chrono::milliseconds(100));
  for (const bool connection_served : served)
    CHECK(connection_served);
  return true;
}

bool PrimaryCapacityRejectsAtProtocolBoundary() {
  const std::wstring suffix =
      L"primary-capacity-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  RuntimeService service;
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  constexpr size_t kConnections = kRuntimeClientCapacity + 1;
  std::vector<std::thread> servers;
  std::vector<uint8_t> served(kConnections, 0);
  std::vector<std::string> server_errors(kConnections);
  servers.reserve(kConnections);
  for (size_t index = 0; index < kConnections; ++index) {
    servers.emplace_back([&, index] {
      RuntimePipeServer server;
      served[index] =
          server.ServeOnce(endpoint, &service, ServerFault::None,
                           std::chrono::seconds(3), &server_errors[index])
              ? 1
              : 0;
    });
  }
  Sleep(100);

  std::vector<HANDLE> connections;
  connections.reserve(kConnections);
  bool transport_ok = true;
  bool first_seventeen_ok = true;
  bool capacity_reply_ok = false;
  for (size_t index = 0; index < kConnections; ++index) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto connect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < connect_deadline) {
      pipe = CreateFileW(endpoint.name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe != INVALID_HANDLE_VALUE)
        break;
      WaitNamedPipeW(endpoint.name.c_str(), 20);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
      transport_ok = false;
      break;
    }
    connections.push_back(pipe);
    Frame hello = Hello(5000 + index);
    hello.correlation.client_id = 10000 + index;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Frame reply;
    const bool exchanged =
        pipe_io::WriteFrame(pipe, hello, deadline, &error) ==
            pipe_io::Result::Ok &&
        pipe_io::ReadFrame(pipe, &reply, deadline, &error) ==
            pipe_io::Result::Ok &&
        reply.command == Command::Hello &&
        reply.correlation == hello.correlation;
    if (!exchanged) {
      transport_ok = false;
      break;
    }
    if (index < 17)
      first_seventeen_ok = first_seventeen_ok && reply.status == Status::Ok;
    if (index < kRuntimeClientCapacity)
      transport_ok = transport_ok && reply.status == Status::Ok;
    else
      capacity_reply_ok = reply.status == Status::Unavailable;
  }

  for (HANDLE pipe : connections)
    CloseHandle(pipe);
  for (std::thread &server : servers)
    server.join();
  service.Stop();
  for (size_t index = 0; index < served.size(); ++index) {
    if (!served[index])
      std::fprintf(stderr, "capacity server %zu failed: %s\n", index,
                   server_errors[index].c_str());
    transport_ok = transport_ok && served[index];
  }
  CHECK(transport_ok);
  CHECK(first_seventeen_ok);
  CHECK(capacity_reply_ok);
  return true;
}

bool ConcurrentRuntimeClients() {
  const std::wstring suffix =
      L"concurrent-runtime-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process, 4, true));

  PipeRuntimePort first;
  PipeRuntimePort second;
  Frame first_hello = Hello(470);
  first_hello.correlation.client_id = 301;
  Frame second_hello = Hello(480);
  second_hello.correlation.client_id = 302;
  const bool first_connected =
      first.Connect(endpoint, RuntimePath(), first_hello.correlation,
                    std::chrono::seconds(2), &error);
  const bool second_connected =
      second.Connect(endpoint, RuntimePath(), second_hello.correlation,
                     std::chrono::seconds(2), &error);
  first.Stop();
  second.Stop();

  CHECK(first_connected);
  CHECK(second_connected);
  CHECK(FinishRuntime(&process));
  return true;
}

bool WrongProcessHelloCannotInvalidateOwnerDelivery() {
  const std::wstring suffix =
      L"owner-binding-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));

  RuntimeService service;
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  std::array<bool, 2> served{};
  std::array<std::string, 2> server_errors;
  std::array<std::thread, 2> servers;
  for (size_t index = 0; index < servers.size(); ++index) {
    servers[index] = std::thread([&, index] {
      RuntimePipeServer server;
      served[index] =
          server.ServeOnce(endpoint, &service, ServerFault::None,
                           std::chrono::seconds(3), &server_errors[index]);
    });
  }
  Sleep(25);

  PipeRuntimePort owner;
  constexpr uint64_t generation = 485;
  const Correlation identity = Hello(generation).correlation;
  CHECK(owner.Connect(endpoint, ModulePath(), identity,
                      std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(owner.Call(std::move(open), kHardCallDeadline).status == Status::Ok);

  Frame key = Request(Command::ProcessKey, generation, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &key.payload));
  const DeliveryReference reference{key.command, key.correlation};
  CHECK(owner
            .Prepare(std::move(key),
                     std::chrono::steady_clock::now() + kHardCallDeadline)
            .status == Status::Prepared);

  // A distinct process presents the exact same logical correlation. Its Hello
  // must be rejected, and that rejected physical connection must not run
  // InvalidateConnection against the legitimate owner's session.
  CHECK(RunWrongOwnerChild(suffix, identity));
  DeliveryResult delivered =
      owner.Claim(reference,
                  std::chrono::steady_clock::now() + kHardCallDeadline);
  CHECK(delivered.status == Status::Prepared);
  delivered = owner.ExecutePrepared(
      reference, std::chrono::steady_clock::now() + kHardCallDeadline);
  CHECK(delivered.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(delivered.final_reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");
  CHECK(owner.Ack(reference, std::chrono::steady_clock::now() +
                                 kHardCallDeadline)
            .status == Status::Ok);

  Frame next = Request(Command::ProcessKey, generation, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &next.payload));
  const CallResult next_result = DeliveredCall(owner, std::move(next));
  CHECK(next_result.status == Status::Ok);
  CHECK(DecodeComposition(next_result.reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "ni");

  owner.Stop();
  for (std::thread &server : servers)
    server.join();
  service.Stop();
  for (size_t index = 0; index < served.size(); ++index) {
    if (!served[index])
      std::fprintf(stderr, "owner server %zu failed: %s\n", index,
                   server_errors[index].c_str());
    CHECK(served[index]);
  }
  return true;
}

bool UiStateFailureDoesNotOccupyOrPoisonProcessKey() {
  const std::wstring suffix =
      L"ui-state-isolation-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"ui-hang", &process, 2, true));

  PipeRuntimePort port;
  constexpr uint64_t generation = 490;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));

  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kHardCallDeadline).status == Status::Ok);

  Frame ui = Request(Command::UpdateUiState, generation, 2);
  CHECK(EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true, {1, 2}},
                      &ui.payload, &error));
  port.Post(std::move(ui));
  Sleep(10);

  Frame first_key = Request(Command::ProcessKey, generation, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &first_key.payload));
  const CallResult first =
      DeliveredCall(port, std::move(first_key));
  const bool first_succeeded = first.status == Status::Ok;
  const bool first_stayed_ready = port.state() == ChannelState::Ready;

  // Let the injected UiState transport failure finish. It must not invalidate
  // the still-active key connection or its session generation.
  Sleep(300);
  Frame second_key = Request(Command::ProcessKey, generation, 4);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &second_key.payload));
  const CallResult second =
      DeliveredCall(port, std::move(second_key));
  const bool second_succeeded = second.status == Status::Ok;
  const bool second_stayed_ready = port.state() == ChannelState::Ready;

  port.Stop();
  const bool runtime_finished = FinishRuntime(&process);
  CHECK(first_succeeded);
  CHECK(first_stayed_ready);
  CHECK(second_succeeded);
  CHECK(second_stayed_ready);
  CHECK(runtime_finished);
  return true;
}

bool UiStateTransportReconnectsAfterTransientFailure() {
  const std::wstring suffix =
      L"ui-state-reconnect-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);

  RuntimeService service;
  RecordingSink sink;
  service.SetSnapshotSink(&sink);
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  std::atomic<bool> primary_served{false};
  std::atomic<bool> first_ui_served{false};
  std::atomic<bool> second_ui_served{false};
  std::string primary_error;
  std::string first_ui_error;
  std::string second_ui_error;
  std::thread primary([&] {
    RuntimePipeServer server;
    primary_served.store(server.ServeOnce(
        endpoint, &service, ServerFault::None, std::chrono::seconds(5),
        &primary_error));
  });
  std::thread first_ui([&] {
    RuntimePipeServer server;
    first_ui_served.store(server.ServeOnce(
        ui_endpoint, &service, ServerFault::UiHang,
        std::chrono::seconds(5), &first_ui_error, 0, nullptr, true));
  });
  Sleep(25);

  PipeRuntimePort port;
  constexpr uint64_t generation = 491;
  const bool connected =
      port.Connect(endpoint, ModulePath(), Hello(generation).correlation,
                   std::chrono::seconds(2), &error);
  bool opened = false;
  bool first_posted = false;
  bool first_applied = false;
  if (connected) {
    Frame open = Request(Command::OpenSession, generation, 1);
    opened = EncodeOpenSession("test", &open.payload, &error) &&
             port.Call(std::move(open), kHardCallDeadline).status == Status::Ok;
  }
  if (opened) {
    Frame ui = Request(Command::UpdateUiState, generation, 2);
    first_posted =
        EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true, {1, 2}},
                      &ui.payload, &error);
    if (first_posted) {
      port.Post(std::move(ui));
      first_applied =
          sink.WaitForUiSequence(2, std::chrono::seconds(1));
    }
  }
  first_ui.join();

  bool second_posted = false;
  bool latest_posted = false;
  if (first_applied) {
    Frame second = Request(Command::UpdateUiState, generation, 3);
    second_posted =
        EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true, {1, 3}},
                      &second.payload, &error);
    if (second_posted)
      port.Post(std::move(second));

    Frame latest = Request(Command::UpdateUiState, generation, 4);
    latest_posted =
        EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true, {1, 4}},
                      &latest.payload, &error);
    if (latest_posted)
      port.Post(std::move(latest));
  }

  std::thread second_ui([&] {
    RuntimePipeServer server;
    second_ui_served.store(server.ServeOnce(
        ui_endpoint, &service, ServerFault::None,
        std::chrono::seconds(2), &second_ui_error, 0, nullptr, true));
  });
  Sleep(25);

  bool recovered = false;
  if (latest_posted)
    recovered = sink.WaitForUiSequence(4, std::chrono::seconds(2));

  port.Stop();
  primary.join();
  second_ui.join();
  service.SetSnapshotSink(nullptr);
  service.Stop();

  if (!primary_served.load())
    std::fprintf(stderr, "primary server failed: %s\n",
                 primary_error.c_str());
  if (!first_ui_served.load())
    std::fprintf(stderr, "first UI server failed: %s\n",
                 first_ui_error.c_str());
  if (!second_ui_served.load())
    std::fprintf(stderr, "second UI server failed: %s\n",
                 second_ui_error.c_str());
  CHECK(connected);
  CHECK(opened);
  CHECK(first_posted);
  CHECK(first_applied);
  CHECK(second_posted);
  CHECK(latest_posted);
  CHECK(recovered);
  CHECK(primary_served.load());
  CHECK(first_ui_served.load());
  CHECK(second_ui_served.load());
  return true;
}

bool TransientBusyKeepsKeyChannelReady() {
  const std::wstring suffix =
      L"transient-busy-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  RuntimeService service;
  BlockingSink sink;
  service.SetSnapshotSink(&sink);
  CHECK(service.Start(EnginePath().c_str(), "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  std::array<bool, 2> served{};
  std::array<std::string, 2> server_errors;
  std::array<std::thread, 2> servers;
  for (size_t index = 0; index < servers.size(); ++index) {
    servers[index] = std::thread([&, index] {
      RuntimePipeServer server;
      const PipeEndpoint &active_endpoint =
          index == 0 ? endpoint : ui_endpoint;
      served[index] = server.ServeOnce(
          active_endpoint, &service, ServerFault::None,
          std::chrono::seconds(2), &server_errors[index], 0, nullptr,
          index != 0);
    });
  }
  Sleep(25);

  PipeRuntimePort port;
  constexpr uint64_t generation = 495;
  const bool connected =
      port.Connect(endpoint, ModulePath(), Hello(generation).correlation,
                   std::chrono::seconds(2), &error);
  bool opened = false;
  if (connected) {
    Frame open = Request(Command::OpenSession, generation, 1);
    opened = EncodeOpenSession("test", &open.payload, &error) &&
             port.Call(std::move(open), kHardCallDeadline).status == Status::Ok;
  }

  std::atomic<Status> blocker_status{Status::Unavailable};
  std::thread blocker;
  bool entered = false;
  CallResult busy_result;
  bool busy_ready = false;
  CallResult retry_result;
  bool retry_ready = false;
  if (opened) {
    sink.Arm();
    Frame blocking_key = Request(Command::ProcessKey, generation, 2);
    if (EncodeKeyEvent({0x7b, 0, 0, 1, 1}, &blocking_key.payload)) {
      blocker = std::thread([&] {
        blocker_status.store(service.Dispatch(blocking_key).status);
      });
      entered = sink.WaitUntilEntered(std::chrono::milliseconds(250));
    }
    if (entered) {
      Frame busy_key = Request(Command::ProcessKey, generation, 3);
      if (EncodeKeyEvent({0x7b, 0, 0, 1, 2}, &busy_key.payload)) {
        busy_result = DeliveredCall(port, std::move(busy_key));
        busy_ready = port.state() == ChannelState::Ready;
      }
    }
  }

  sink.Release();
  if (blocker.joinable())
    blocker.join();
  if (entered && blocker_status.load() == Status::Ok) {
    Frame retry_key = Request(Command::ProcessKey, generation, 3);
    if (EncodeKeyEvent({0x7b, 0, 0, 1, 3}, &retry_key.payload)) {
      retry_result = DeliveredCall(port, std::move(retry_key));
      retry_ready = port.state() == ChannelState::Ready;
    }
  }

  port.Stop();
  for (std::thread &server : servers)
    server.join();
  service.SetSnapshotSink(nullptr);
  service.Stop();

  CHECK(connected);
  CHECK(opened);
  CHECK(entered);
  CHECK(busy_result.status == Status::Unavailable);
  CHECK(busy_ready);
  CHECK(blocker_status.load() == Status::Ok);
  CHECK(retry_result.status == Status::Ok);
  CHECK(retry_ready);
  for (const bool connection_served : served)
    CHECK(connection_served);
  return true;
}

bool UiStateFloodKeepsOneThousandKeysWithinBudget() {
  const std::wstring suffix =
      L"ui-flood-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process));

  PipeRuntimePort port;
  constexpr uint64_t generation = 497;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kHardCallDeadline).status == Status::Ok);

  std::atomic<bool> stop{false};
  std::atomic<bool> encode_failed{false};
  std::atomic<uint64_t> next_sequence{2};
  std::atomic<uint64_t> posts{0};
  std::thread producer([&] {
    std::string producer_error;
    while (!stop.load()) {
      const uint64_t sequence = next_sequence.fetch_add(1);
      Frame ui = Request(Command::UpdateUiState, generation, sequence);
      const int32_t offset = static_cast<int32_t>(sequence % 1000);
      if (!EncodeUiState({{offset, 480, offset + 2, 504},
                          {0, 0, 1920, 1080}, 144, true, true, true,
                          {sequence, sequence ^ 0xa5a5a5a5u}},
                         &ui.payload, &producer_error)) {
        encode_failed.store(true);
        break;
      }
      port.Post(std::move(ui));
      posts.fetch_add(1);
      SwitchToThread();
    }
  });

  while (posts.load() < 1000 && !encode_failed.load())
    SwitchToThread();

  bool warmup_ok = true;
  for (uint64_t index = 0; index < 20; ++index) {
    Frame key = Request(Command::ProcessKey, generation,
                        next_sequence.fetch_add(1));
    if (!EncodeKeyEvent({0x7b, 0, 0, 1, index}, &key.payload) ||
        DeliveredCall(port, std::move(key)).status != Status::Ok) {
      warmup_ok = false;
      break;
    }
  }

  std::vector<int64_t> latencies_us;
  latencies_us.reserve(1000);
  size_t failures = 0;
  for (uint64_t index = 0; index < 1000; ++index) {
    Frame key = Request(Command::ProcessKey, generation,
                        next_sequence.fetch_add(1));
    if (!EncodeKeyEvent({0x7b, 0, 0, 1, index + 20}, &key.payload)) {
      ++failures;
      continue;
    }
    const auto started = std::chrono::steady_clock::now();
    const CallResult result = DeliveredCall(port, std::move(key));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    latencies_us.push_back(elapsed.count());
    if (result.status != Status::Ok)
      ++failures;
  }

  stop.store(true);
  producer.join();
  const bool stayed_ready = port.state() == ChannelState::Ready;
  port.Stop();
  const bool runtime_finished = FinishRuntime(&process);

  std::sort(latencies_us.begin(), latencies_us.end());
  const int64_t p99_us = latencies_us.size() == 1000
                             ? latencies_us[989]
                             : kHardCallDeadline.count() * 1000 + 1;
  const int64_t max_us = latencies_us.size() == 1000
                             ? latencies_us.back()
                             : kHardCallDeadline.count() * 1000 + 1;
  std::printf("ui_flood: posts=%llu failures=%zu p99_us=%lld max_us=%lld\n",
              static_cast<unsigned long long>(posts.load()), failures,
              static_cast<long long>(p99_us), static_cast<long long>(max_us));

  CHECK(!encode_failed.load());
  CHECK(posts.load() >= 1000);
  CHECK(warmup_ok);
  CHECK(failures == 0);
  CHECK(stayed_ready);
  CHECK(p99_us <= 16000);
  CHECK(max_us <= 50000);
  CHECK(runtime_finished);
  return true;
}

bool FaultCheck(std::wstring_view name, std::wstring_view fault,
                uint64_t generation, bool large_write) {
  const std::wstring suffix =
      std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, fault, &process));
  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  Frame request = Request(Command::ProcessKey, generation, 1);
  if (large_write)
    request.payload.resize(kMaxFrameSize - kHeaderSize, 0x41);
  const CallResult first = port.Call(std::move(request), kHardCallDeadline);
  CHECK(first.status == Status::Timeout || first.status == Status::Unavailable);
  CHECK(first.elapsed <= kHardCallDeadline);
  CHECK(port.state() == ChannelState::OpenCircuit);
  // A business request can have been prepared before the transport fault.
  // Retiring the physical pipe must preserve the logical generation so Claim
  // or cancellation can address that exact delivery after reconnect.
  CHECK(port.connection_generation() == generation);

  Frame later = Hello(generation);
  const auto started = std::chrono::steady_clock::now();
  CHECK(port.Call(std::move(later), kHardCallDeadline).status ==
        Status::Unavailable);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(10));
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool ExecuteDisconnectRecoversExactDelivery(
    std::wstring_view name, std::wstring_view fault, uint64_t generation,
    Status expected_claim_status) {
  const std::wstring suffix =
      std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, fault, &process, 2, false));

  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, generation, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kSessionOpenDeadline).status == Status::Ok);

  Frame key_n = Request(Command::ProcessKey, generation, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &key_n.payload));
  const DeliveryReference reference{key_n.command, key_n.correlation};
  CHECK(port.Prepare(std::move(key_n),
                     std::chrono::steady_clock::now() + kHardCallDeadline)
            .status == Status::Prepared);
  const DeliveryResult ambiguous =
      port.ExecutePrepared(reference,
                           std::chrono::steady_clock::now() +
                               kHardCallDeadline);
  CHECK(ambiguous.status == Status::Unavailable ||
        ambiguous.status == Status::Timeout);
  CHECK(port.connection_generation() == generation);
  port.Stop();

  CHECK(port.Connect(endpoint, RuntimePath(), Hello(generation).correlation,
                     std::chrono::seconds(2), &error));
  DeliveryResult recovered =
      port.Claim(reference,
                 std::chrono::steady_clock::now() + kHardCallDeadline);
  CHECK(recovered.status == expected_claim_status);
  if (recovered.status == Status::Prepared) {
    recovered =
        port.ExecutePrepared(reference,
                             std::chrono::steady_clock::now() +
                                 kHardCallDeadline);
  }
  CHECK(recovered.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(recovered.final_reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");
  CHECK(port.Ack(reference, std::chrono::steady_clock::now() +
                                kHardCallDeadline)
            .status == Status::Ok);

  Frame key_i = Request(Command::ProcessKey, generation, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &key_i.payload));
  const CallResult current = DeliveredCall(port, std::move(key_i));
  CHECK(current.status == Status::Ok);
  CHECK(DecodeComposition(current.reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "ni");

  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool LatePreparedDeliveryReturnsOnSameGeneration() {
  const std::wstring suffix =
      L"late-reconnect-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"late", &process, 2, false));
  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(700).correlation,
                     std::chrono::seconds(2), &error));
  Frame old_open = Request(Command::OpenSession, 700, 1);
  CHECK(EncodeOpenSession("test", &old_open.payload, &error));
  CHECK(port.Call(std::move(old_open), kHardCallDeadline).status == Status::Ok);
  Frame old_key = Request(Command::ProcessKey, 700, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &old_key.payload));
  const DeliveryReference reference{old_key.command, old_key.correlation};
  const CallResult prepared =
      port.Prepare(std::move(old_key),
                   std::chrono::steady_clock::now() + kHardCallDeadline);
  CHECK(prepared.status == Status::Timeout);
  CHECK(port.connection_generation() == 700);
  port.Stop();

  // The first server deliberately publishes Prepare after the caller's
  // deadline. Wait until that physical connection has retired, then reconnect
  // with the same logical generation and finish the exact delivery.
  Sleep(300);
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(700).correlation,
                     std::chrono::seconds(2), &error));
  DeliveryResult recovered =
      port.Claim(reference,
                 std::chrono::steady_clock::now() + kHardCallDeadline);
  CHECK(recovered.status == Status::Prepared);
  recovered =
      port.ExecutePrepared(reference,
                           std::chrono::steady_clock::now() +
                               kHardCallDeadline);
  CHECK(recovered.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(recovered.final_reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");
  CHECK(port.Ack(reference, std::chrono::steady_clock::now() +
                                kHardCallDeadline)
            .status == Status::Ok);

  Frame current_key = Request(Command::ProcessKey, 700, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &current_key.payload));
  const CallResult current = DeliveredCall(port, std::move(current_key));
  CHECK(current.status == Status::Ok);
  CHECK(DecodeComposition(current.reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "ni");
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool DuplicateOpenSessionIsIdempotent() {
  const std::wstring suffix =
      L"duplicate-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"none", &process));
  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(800).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, 800, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(port.Call(std::move(open), kHardCallDeadline).status == Status::Ok);
  Frame duplicate = Request(Command::OpenSession, 800, 1);
  CHECK(EncodeOpenSession("test", &duplicate.payload, &error));
  CHECK(port.Call(std::move(duplicate), kSessionOpenDeadline).status ==
        Status::Ok);
  Frame malformed = Request(Command::OpenSession, 800, 1);
  malformed.payload = {1};
  CHECK(port.Call(std::move(malformed), kSessionOpenDeadline).status ==
        Status::InvalidFrame);
  Frame changed = Request(Command::OpenSession, 800, 1);
  CHECK(EncodeOpenSession("different", &changed.payload, &error));
  CHECK(port.Call(std::move(changed), kSessionOpenDeadline).status ==
        Status::InvalidFrame);
  Frame jumped = Request(Command::OpenSession, 800, 2);
  CHECK(EncodeOpenSession("test", &jumped.payload, &error));
  CHECK(port.Call(std::move(jumped), kSessionOpenDeadline).status ==
        Status::StaleRequest);
  Frame key = Request(Command::ProcessKey, 800, 2);
  CHECK(EncodeKeyEvent({'N', 0, 0, 1, 1}, &key.payload));
  CHECK(DeliveredCall(port, std::move(key)).status == Status::Ok);
  CHECK(port.state() == ChannelState::Ready);
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool StopRetiresInFlightOffControlPath() {
  const std::wstring suffix =
      L"stop-retire-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"no-reply", &process));
  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(900).correlation,
                     std::chrono::seconds(2), &error));

  Frame request = Request(Command::ProcessKey, 900, 1);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &request.payload));
  std::atomic<Status> status{Status::Ok};
  std::thread caller([&] {
    status.store(port.Call(std::move(request), kHardCallDeadline).status);
  });
  Sleep(10);
  const auto started = std::chrono::steady_clock::now();
  port.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  caller.join();
  CHECK(elapsed <= kHardCallDeadline + std::chrono::milliseconds(15));
  CHECK(status.load() != Status::Ok);
  CHECK(FinishRuntime(&process));
  return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--extended-hello")
    return ExtendedHelloRoundtrip() ? 0 : 1;
  if (argc == 4 &&
      std::wstring_view(argv[1]) == L"--control-intruder") {
    PipeEndpoint endpoint;
    std::string error;
    if (!BuildCurrentPipeEndpoint(argv[2], &endpoint, &error))
      return 2;
    ControlResult result;
    const bool accepted =
        RunControlClient(endpoint, argv[3], Command::ControlDeploy,
                         std::chrono::seconds(2), &result, &error);
    return accepted ? 1 : 0;
  }
  if (argc == 6 && std::wstring_view(argv[1]) == L"--wrong-owner") {
    PipeEndpoint endpoint;
    std::string error;
    if (!BuildCurrentPipeEndpoint(argv[2], &endpoint, &error))
      return 2;
    Correlation identity{};
    identity.client_id = _wcstoui64(argv[3], nullptr, 10);
    identity.activation_generation = _wcstoui64(argv[4], nullptr, 10);
    identity.connection_generation = _wcstoui64(argv[5], nullptr, 10);
    PipeRuntimePort intruder;
    const bool connected =
        intruder.Connect(endpoint, ModulePath(), identity,
                         std::chrono::seconds(2), &error);
    intruder.Stop();
    return connected ? 1 : 0;
  }
  if (argc != 1)
    return 2;
  if (!PipeSecurityAllowsRestrictedClients() ||
      !ExtendedHelloRoundtrip() || !NormalRoundtrip() ||
      !AbsolutePreviewSelectionRoundtrip() ||
      !WrongPeerRejected() ||
      !ConnectFailureIsOffHotPath() ||
      !OptionalUiPipeDoesNotDelayPrimaryReadiness() ||
      !OptionalUiPipeRecoversWhenItAppears() ||
      !UiEndpointRejectsBusinessCommands() || !ConcurrentClients() ||
      !PrimaryCapacityRejectsAtProtocolBoundary() ||
      !ConcurrentRuntimeClients() ||
      !WrongProcessHelloCannotInvalidateOwnerDelivery() ||
      !UiStateFailureDoesNotOccupyOrPoisonProcessKey() ||
      !UiStateTransportReconnectsAfterTransientFailure() ||
      !TransientBusyKeepsKeyChannelReady() ||
      !UiStateFloodKeepsOneThousandKeysWithinBudget() ||
      !FaultCheck(L"write-hang", L"no-read", 500, true) ||
      !FaultCheck(L"read-hang", L"no-reply", 510, false) ||
      !FaultCheck(L"engine-hang", L"engine-hang", 520, false) ||
      !FaultCheck(L"malformed", L"malformed", 540, false) ||
      !FaultCheck(L"wrong-version", L"wrong-version", 545, false) ||
      !FaultCheck(L"disconnect", L"disconnect", 550, false) ||
      !ExecuteDisconnectRecoversExactDelivery(
          L"disconnect-before-execute", L"disconnect-before-execute", 560,
          Status::Prepared) ||
      !ExecuteDisconnectRecoversExactDelivery(
          L"disconnect-after-dispatch", L"disconnect-after-dispatch", 570,
          Status::Ok) ||
      !LatePreparedDeliveryReturnsOnSameGeneration() ||
      !DuplicateOpenSessionIsIdempotent() ||
      !StopRetiresInFlightOffControlPath()) {
    return 1;
  }
  std::printf("pipe_roundtrip_selfcheck: OK\n");
  return 0;
}
