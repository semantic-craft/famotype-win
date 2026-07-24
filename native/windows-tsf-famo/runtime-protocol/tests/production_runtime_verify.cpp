// Manual process-level verification for the production runtime and real Rime.
//
//   production_runtime_verify <data_root> <schema_id>
//
// The supplied data root must already contain a deployed schema. The tool
// launches the sibling FamoRuntime.exe on a unique pipe, exercises the v1 wire
// commands, and terminates only that explicitly-owned process when finished.
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "famo_runtime_pipe.h"
#include "famo_runtime_control.h"

using namespace famo::runtime;

namespace {

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

std::wstring ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::wstring RuntimePath() {
  return ModuleDirectory() + L"\\FamoRuntime.exe";
}

bool Utf8(std::wstring_view source, std::string *target) {
  if (!target || source.empty())
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

class OwnedRuntimeProcess {
public:
  ~OwnedRuntimeProcess() { Stop(); }

  bool Start(std::wstring_view suffix, std::wstring_view data_root) {
    std::wstring command = L"\"" + RuntimePath() +
                           L"\" --endpoint-suffix " + std::wstring(suffix) +
                           L" --workers 2 --data-root \"" +
                           std::wstring(data_root) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, ModuleDirectory().c_str(),
                        &startup, &process_)) {
      return false;
    }
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;
    return true;
  }

  void Stop() {
    if (!process_.hProcess)
      return;
    if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process_.hProcess, 0);
      WaitForSingleObject(process_.hProcess, 1000);
    }
    CloseHandle(process_.hProcess);
    process_.hProcess = nullptr;
  }

  bool WaitForExit(std::chrono::milliseconds timeout) const {
    return process_.hProcess &&
           WaitForSingleObject(process_.hProcess,
                               static_cast<DWORD>(timeout.count())) ==
               WAIT_OBJECT_0;
  }

private:
  PROCESS_INFORMATION process_{};
};

Frame Request(Command command, uint64_t sequence,
              uint64_t connection_generation = 1,
              uint64_t session_generation = 1) {
  Frame frame;
  frame.command = command;
  frame.correlation = {0x46414d4f, 1, connection_generation, 1,
                       session_generation, sequence};
  return frame;
}

Correlation HelloCorrelation(uint64_t connection_generation = 1) {
  Correlation correlation =
      Request(Command::Hello, 0, connection_generation).correlation;
  correlation.session_id = 0;
  correlation.session_generation = 0;
  return correlation;
}

bool DecodeOk(CallResult result, Composition *composition,
              std::string *error) {
  return result.status == Status::Ok &&
         DecodeComposition(result.reply.payload, composition, error);
}

bool HasMultibyteCandidate(const Composition &composition) {
  for (const Candidate &candidate : composition.candidates) {
    for (const unsigned char byte : candidate.text) {
      if (byte >= 0x80)
        return true;
    }
  }
  return false;
}

bool Run(std::wstring_view data_root, std::string_view schema,
         bool deploy_while_typing) {
  const std::wstring suffix =
      L"production-verify-" + std::to_wstring(GetCurrentProcessId());
  PipeEndpoint endpoint;
  PipeEndpoint control_endpoint;
  std::string error;
  CHECK(BuildCurrentPipeEndpoint(suffix, &endpoint, &error));
  CHECK(BuildCurrentPipeEndpoint(suffix + L"-control", &control_endpoint,
                                 &error));

  OwnedRuntimeProcess runtime;
  CHECK(runtime.Start(suffix, data_root));

  uint64_t connection_generation = 1;
  if (deploy_while_typing) {
    PipeRuntimePort stress_port;
    CHECK(stress_port.Connect(endpoint, RuntimePath(), HelloCorrelation(1),
                              std::chrono::seconds(5), &error));
    Frame stress_open = Request(Command::OpenSession, 1);
    CHECK(EncodeOpenSession(schema, &stress_open.payload, &error));
    CHECK(stress_port.Call(std::move(stress_open), kSessionOpenDeadline).status ==
          Status::Ok);

    bool control_ok = false;
    ControlResult deploy_result;
    std::string control_error;
    std::thread deploy([&] {
      control_ok = RunControlClient(
          control_endpoint, RuntimePath(), Command::ControlDeploy,
          std::chrono::minutes(2), &deploy_result, &control_error);
    });
    CallResult failed_open;
    for (uint64_t sequence = 2; sequence < 1002; ++sequence) {
      Frame key = Request(Command::ProcessKey, sequence);
      CHECK(EncodeKeyEvent({'n', 0, 0, 1, sequence}, &key.payload));
      CallResult call = stress_port.Call(std::move(key), kHardCallDeadline);
      if (call.status != Status::Ok) {
        failed_open = call;
        break;
      }
      Sleep(1);
    }
    deploy.join();
    CHECK(control_ok);
    CHECK(deploy_result.state == ControlState::Succeeded);
    CHECK(deploy_result.readiness == RuntimeReadiness::Ready);
    CHECK(failed_open.status == Status::Unavailable);
    CHECK(failed_open.elapsed <= kHardCallDeadline);
    std::printf("deploy_fail_open_ms=%lld generation=%llu\n",
                static_cast<long long>(failed_open.elapsed.count()),
                static_cast<unsigned long long>(deploy_result.engine_generation));
    stress_port.Stop();
    connection_generation = 2;
  }

  PipeRuntimePort port;
  CHECK(port.Connect(endpoint, RuntimePath(),
                     HelloCorrelation(connection_generation),
                     std::chrono::seconds(5), &error));

  const auto request = [connection_generation](Command command,
                                                uint64_t sequence) {
    return Request(command, sequence, connection_generation,
                   connection_generation);
  };

  Frame open = request(Command::OpenSession, 1);
  CHECK(EncodeOpenSession(schema, &open.payload, &error));
  const CallResult open_result =
      port.Call(std::move(open), kSessionOpenDeadline);
  if (open_result.status != Status::Ok) {
    std::fprintf(stderr, "OpenSession failed: status=%u elapsed=%lldms %s\n",
                 static_cast<unsigned>(open_result.status),
                 static_cast<long long>(open_result.elapsed.count()),
                 error.c_str());
    return false;
  }

  Composition composition;
  Frame key = request(Command::ProcessKey, 2);
  CHECK(EncodeKeyEvent({'n', 0, 0, 1, 1}, &key.payload));
  CHECK(DecodeOk(port.Call(std::move(key), kHardCallDeadline), &composition,
                 &error));
  CHECK(composition.handled && composition.preedit == "n");

  key = request(Command::ProcessKey, 3);
  CHECK(EncodeKeyEvent({'i', 0, 0, 1, 2}, &key.payload));
  CHECK(DecodeOk(port.Call(std::move(key), kHardCallDeadline), &composition,
                 &error));
  CHECK(composition.handled && composition.preedit == "ni");
  CHECK(composition.candidates.size() > 1);
  CHECK(HasMultibyteCandidate(composition));

  Frame ui = request(Command::UpdateUiState, 4);
  CHECK(EncodeUiState({{640, 480, 642, 504}, {0, 0, 1920, 1080}, 144, true,
                       true, true},
                      &ui.payload, &error));
  port.Post(std::move(ui));
  Sleep(20);

  Frame highlight = request(Command::HighlightCandidate, 5);
  CHECK(EncodeCandidateIndex(1, &highlight.payload));
  CHECK(DecodeOk(port.Call(std::move(highlight), kHardCallDeadline),
                 &composition, &error));
  CHECK(composition.preedit == "ni");

  Frame page = request(Command::ChangePage, 6);
  CHECK(EncodePageDirection(false, &page.payload));
  CHECK(DecodeOk(port.Call(std::move(page), kHardCallDeadline), &composition,
                 &error));

  // X11/Rime BackSpace keysym. This is the value produced by the TSF VK
  // translation boundary, not the raw Windows VK_BACK value.
  key = request(Command::ProcessKey, 7);
  CHECK(EncodeKeyEvent({0xff08, 0, 0, 1, 3}, &key.payload));
  CHECK(DecodeOk(port.Call(std::move(key), kHardCallDeadline), &composition,
                 &error));
  CHECK(composition.preedit.size() < 2);

  Frame clear = request(Command::ClearComposition, 8);
  CHECK(DecodeOk(port.Call(std::move(clear), kHardCallDeadline), &composition,
                 &error));
  CHECK(composition.preedit.empty());

  for (uint64_t sequence = 9; sequence <= 10; ++sequence) {
    key = request(Command::ProcessKey, sequence);
    const uint32_t letter = sequence == 9 ? 'n' : 'i';
    CHECK(EncodeKeyEvent({letter, 0, 0, 1, sequence}, &key.payload));
    CHECK(DecodeOk(port.Call(std::move(key), kHardCallDeadline), &composition,
                   &error));
  }
  Frame commit = request(Command::CommitComposition, 11);
  CHECK(DecodeOk(port.Call(std::move(commit), kHardCallDeadline), &composition,
                 &error));
  CHECK(composition.preedit.empty());
  CHECK(!composition.commit.empty());

  for (uint64_t sequence = 12; sequence <= 13; ++sequence) {
    key = request(Command::ProcessKey, sequence);
    const uint32_t letter = sequence == 12 ? 'n' : 'i';
    CHECK(EncodeKeyEvent({letter, 0, 0, 1, sequence}, &key.payload));
    CHECK(DecodeOk(port.Call(std::move(key), kHardCallDeadline), &composition,
                   &error));
  }
  Frame select = request(Command::SelectCandidate, 14);
  CHECK(EncodeCandidateIndex(0, &select.payload));
  CHECK(DecodeOk(port.Call(std::move(select), kHardCallDeadline), &composition,
                 &error));
  CHECK(!composition.commit.empty());

  if (!deploy_while_typing) {
    Frame close = request(Command::CloseSession, 15);
    CHECK(port.Call(std::move(close), kHardCallDeadline).status == Status::Ok);
    port.Stop();
  }
  ControlResult shutdown_result;
  CHECK(RunControlClient(control_endpoint, RuntimePath(),
                         Command::ControlShutdown, std::chrono::seconds(5),
                         &shutdown_result, &error));
  CHECK(shutdown_result.state == ControlState::Succeeded);
  CHECK(runtime.WaitForExit(std::chrono::seconds(5)));
  port.Stop();
  runtime.Stop();
  return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr,
                 "usage: production_runtime_verify <data_root> <schema_id> "
                 "[--deploy-while-typing]\n");
    return 2;
  }
  std::string schema;
  const bool deploy_while_typing =
      argc == 4 && std::wstring_view(argv[3]) == L"--deploy-while-typing";
  if ((argc == 4 && !deploy_while_typing) || !Utf8(argv[2], &schema) ||
      !Run(argv[1], schema, deploy_while_typing))
    return 1;
  std::printf("production_runtime_verify: OK\n");
  return 0;
}
