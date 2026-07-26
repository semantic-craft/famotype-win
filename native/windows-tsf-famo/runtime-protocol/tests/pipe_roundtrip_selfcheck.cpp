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

#include "famo_runtime_pipe.h"

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
        port.Call(std::move(unhandled), kHardCallDeadline);
    CHECK(immediate.status == Status::Ok);
  }

  Frame key = Request(Command::ProcessKey, generation, 102);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1}, &key.payload));
  CallResult result = port.Call(std::move(key), kHardCallDeadline);
  CHECK(result.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");

  key = Request(Command::ProcessKey, generation, 103);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2}, &key.payload));
  result = port.Call(std::move(key), kHardCallDeadline);
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.preedit == "ni" && !composition.candidates.empty());

  Frame select = Request(Command::SelectCandidate, generation, 104);
  CHECK(EncodeCandidateIndex(0, &select.payload));
  result = port.Call(std::move(select), kHardCallDeadline);
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(composition.commit == "\xe4\xbd\xa0");

  Frame unhandled = Request(Command::ProcessKey, generation, 105);
  CHECK(EncodeKeyEvent({0x7b, 0, 0, 1, 3}, &unhandled.payload));
  result = port.Call(std::move(unhandled), kHardCallDeadline);
  CHECK(result.status == Status::Ok);
  CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  CHECK(!composition.handled);

  Frame close = Request(Command::CloseSession, generation, 106);
  CHECK(port.Call(std::move(close), kHardCallDeadline).status == Status::Ok);
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
    CallResult result = port.Call(std::move(key), kHardCallDeadline);
    CHECK(result.status == Status::Ok);
    CHECK(DecodeComposition(result.reply.payload, &composition, &error));
  }
  CHECK(composition.page_size == 1);
  CHECK(composition.is_last_page == 0);

  Frame stale = Request(Command::SelectCandidateAbsolute, generation, 4);
  CHECK(EncodeAbsoluteCandidateSelection(1, 2, &stale.payload));
  const CallResult stale_result =
      port.Call(std::move(stale), kHardCallDeadline);
  CHECK(stale_result.status == Status::StaleRequest);
  Frame out_of_range =
      Request(Command::SelectCandidateAbsolute, generation, 5);
  CHECK(EncodeAbsoluteCandidateSelection(3, 3, &out_of_range.payload));
  CHECK(port.Call(std::move(out_of_range), kHardCallDeadline).status ==
        Status::InvalidFrame);

  Frame page_two =
      Request(Command::SelectCandidateAbsolute, generation, 6);
  CHECK(EncodeAbsoluteCandidateSelection(1, 3, &page_two.payload));
  CallResult selected = port.Call(std::move(page_two), kHardCallDeadline);
  CHECK(selected.status == Status::Ok);
  CHECK(DecodeComposition(selected.reply.payload, &composition, &error));
  CHECK(composition.commit == "\xe5\xb0\xbc");

  for (uint64_t sequence = 7; sequence <= 8; ++sequence) {
    Frame key = Request(Command::ProcessKey, generation, sequence);
    const uint32_t letter = sequence == 7 ? 'N' : 'I';
    CHECK(EncodeKeyEvent({letter, 0, 0, 1, sequence}, &key.payload));
    selected = port.Call(std::move(key), kHardCallDeadline);
    CHECK(selected.status == Status::Ok);
    CHECK(DecodeComposition(selected.reply.payload, &composition, &error));
  }
  Frame page_three =
      Request(Command::SelectCandidateAbsolute, generation, 9);
  CHECK(EncodeAbsoluteCandidateSelection(2, 8, &page_three.payload));
  selected = port.Call(std::move(page_three), kHardCallDeadline);
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
          std::chrono::seconds(2), &server_errors[index]);
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
                       true, true},
                      &ui.payload, &error));
  port.Post(std::move(ui));
  Sleep(10);

  Frame first_key = Request(Command::ProcessKey, generation, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &first_key.payload));
  const CallResult first =
      port.Call(std::move(first_key), kHardCallDeadline);
  const bool first_succeeded = first.status == Status::Ok;
  const bool first_stayed_ready = port.state() == ChannelState::Ready;

  // Let the injected UiState transport failure finish. It must not invalidate
  // the still-active key connection or its session generation.
  Sleep(300);
  Frame second_key = Request(Command::ProcessKey, generation, 4);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &second_key.payload));
  const CallResult second =
      port.Call(std::move(second_key), kHardCallDeadline);
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
          std::chrono::seconds(2), &server_errors[index]);
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
        busy_result = port.Call(std::move(busy_key), kHardCallDeadline);
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
      retry_result = port.Call(std::move(retry_key), kHardCallDeadline);
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
                          {0, 0, 1920, 1080}, 144, true, true, true},
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
        port.Call(std::move(key), kHardCallDeadline).status != Status::Ok) {
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
    const CallResult result = port.Call(std::move(key), kHardCallDeadline);
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
  CHECK(port.connection_generation() == generation + 1);

  Frame later = Hello(generation + 1);
  const auto started = std::chrono::steady_clock::now();
  CHECK(port.Call(std::move(later), kHardCallDeadline).status ==
        Status::Unavailable);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(10));
  port.Stop();
  CHECK(FinishRuntime(&process));
  return true;
}

bool LateGenerationCannotReturn() {
  const std::wstring suffix =
      L"late-reconnect-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  PROCESS_INFORMATION process{};
  CHECK(SpawnRuntime(suffix, L"late", &process, 4, true));
  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(), Hello(700).correlation,
                     std::chrono::seconds(2), &error));
  Frame old_open = Request(Command::OpenSession, 700, 1);
  CHECK(EncodeOpenSession("test", &old_open.payload, &error));
  CHECK(port.Call(std::move(old_open), kHardCallDeadline).status == Status::Ok);
  Frame old_key = Request(Command::ProcessKey, 700, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &old_key.payload));
  CHECK(port.Call(std::move(old_key), kHardCallDeadline).status ==
        Status::Timeout);
  CHECK(port.connection_generation() == 701);
  port.Stop();

  CHECK(port.Connect(endpoint, RuntimePath(), Hello(701).correlation,
                     std::chrono::seconds(2), &error));
  Frame open = Request(Command::OpenSession, 701, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  const CallResult current = port.Call(std::move(open), kHardCallDeadline);
  CHECK(current.status == Status::Ok);
  CHECK(current.reply.correlation.connection_generation == 701);
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

int main() {
  if (!NormalRoundtrip() || !AbsolutePreviewSelectionRoundtrip() ||
      !WrongPeerRejected() ||
      !ConnectFailureIsOffHotPath() ||
      !OptionalUiPipeDoesNotDelayPrimaryReadiness() || !ConcurrentClients() ||
      !ConcurrentRuntimeClients() ||
      !UiStateFailureDoesNotOccupyOrPoisonProcessKey() ||
      !TransientBusyKeepsKeyChannelReady() ||
      !UiStateFloodKeepsOneThousandKeysWithinBudget() ||
      !FaultCheck(L"write-hang", L"no-read", 500, true) ||
      !FaultCheck(L"read-hang", L"no-reply", 510, false) ||
      !FaultCheck(L"engine-hang", L"engine-hang", 520, false) ||
      !FaultCheck(L"malformed", L"malformed", 540, false) ||
      !FaultCheck(L"disconnect", L"disconnect", 550, false) ||
      !LateGenerationCannotReturn() || !DuplicateOpenSessionIsIdempotent() ||
      !StopRetiresInFlightOffControlPath()) {
    return 1;
  }
  std::printf("pipe_roundtrip_selfcheck: OK\n");
  return 0;
}
