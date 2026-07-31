#include "famo_runtime_pipe.h"
#include "famo_runtime_control.h"

#include <algorithm>
#include <type_traits>
#include <vector>

#include "pipe_io.h"
#include "win_handle.h"

namespace famo::runtime {
namespace {

using win::UniqueHandle;

class StopRegistration {
public:
  StopRegistration(PipeServerStop *stop, HANDLE pipe)
      : stop_(stop), pipe_(pipe) {}
  ~StopRegistration() {
    if (stop_)
      stop_->Unregister(pipe_);
  }

private:
  PipeServerStop *stop_;
  HANDLE pipe_;
};

bool ConnectClient(HANDLE pipe, std::chrono::milliseconds timeout,
                   std::string *error) {
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    if (error)
      *error = "CreateEvent(connect) failed";
    return false;
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  if (ConnectNamedPipe(pipe, &overlapped))
    return true;
  const DWORD connect_error = GetLastError();
  if (connect_error == ERROR_PIPE_CONNECTED)
    return true;
  if (connect_error != ERROR_IO_PENDING) {
    if (error) {
      *error = "ConnectNamedPipe failed: " + std::to_string(connect_error);
    }
    return false;
  }
  const DWORD wait = WaitForSingleObject(
      event.get(), static_cast<DWORD>(std::max<int64_t>(1, timeout.count())));
  if (wait == WAIT_TIMEOUT) {
    CancelIoEx(pipe, &overlapped);
    WaitForSingleObject(event.get(), INFINITE);
    if (error)
      *error = "pipe accept deadline elapsed";
    return false;
  }
  DWORD transferred = 0;
  if (wait != WAIT_OBJECT_0 ||
      !GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
    if (error)
      *error = "pipe accept completion failed";
    return false;
  }
  return true;
}

bool IsDelayedFault(ServerFault fault) {
  return fault == ServerFault::NoReply || fault == ServerFault::EngineHang ||
         fault == ServerFault::OpenSessionDelay ||
         fault == ServerFault::OpenSessionHang ||
         fault == ServerFault::UiHang || fault == ServerFault::LateReply;
}

bool IsCopyFailureFault(ServerFault fault) {
  return fault == ServerFault::CopyFailureAfterMutation ||
         fault == ServerFault::CopyFailureAfterMutationSticky;
}

std::wstring CurrentProcessPath() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  return path;
}

} // namespace

template <typename Service>
bool ServeOnceImpl(const PipeEndpoint &endpoint, Service *service,
                   ServerFault fault,
                   std::chrono::milliseconds accept_timeout,
                   std::string *error,
                   uint32_t fault_after_process_keys, PipeServerStop *stop,
                   bool preserve_ui_only_session,
                   bool ui_only_endpoint,
                   std::atomic<uint32_t> *terminal_abandon_count) {
  if (!service) {
    if (error)
      *error = "runtime service is required";
    return false;
  }
  SECURITY_ATTRIBUTES attributes{};
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!BuildPipeSecurity(endpoint, &attributes, &descriptor, error))
    return false;
  HANDLE raw_pipe = CreateNamedPipeW(endpoint.name.c_str(),
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                         PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0,
                                     &attributes);
  LocalFree(descriptor);
  if (raw_pipe == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "CreateNamedPipe failed: " + std::to_string(GetLastError());
    }
    return false;
  }
  UniqueHandle pipe(raw_pipe);
  if (stop && !stop->Register(pipe.get())) {
    if (error)
      *error = "pipe server is stopping";
    return false;
  }
  StopRegistration registration(stop, pipe.get());
  PipeClientIdentity client_identity;
  const std::wstring expected_client =
      std::is_same_v<Service, RuntimeControlService>
          ? CurrentProcessPath()
          : std::wstring{};
  if constexpr (std::is_same_v<Service, RuntimeControlService>) {
    if (expected_client.empty()) {
      if (error)
        *error = "control server image path is unavailable";
      return false;
    }
  }
  if (!ConnectClient(pipe.get(), accept_timeout, error) ||
      !VerifyPipeClient(
          pipe.get(), endpoint, error, &client_identity, expected_client)) {
    return false;
  }

  Correlation last_correlation{};
  bool have_correlation = false;
  bool hello_complete = false;
  bool authenticated = false;
  bool only_ui_state = true;
  bool ok = true;
  bool copy_failure_injected = false;
  uint32_t process_key_count = 0;
  uint32_t open_session_count = 0;
  try {
  for (;;) {
    if (fault == ServerFault::NoRead && hello_complete &&
        process_key_count >= fault_after_process_keys) {
      Sleep(250);
      break;
    }
    Frame request;
    const pipe_io::Result read =
        pipe_io::ReadFrame(pipe.get(), &request, pipe_io::Deadline::max(), error);
    if (read == pipe_io::Result::Disconnected)
      break;
    if (read != pipe_io::Result::Ok) {
      ok = false;
      break;
    }
    if ((!hello_complete && request.command != Command::Hello) ||
        (hello_complete &&
         (request.correlation.client_id != last_correlation.client_id ||
          request.correlation.activation_generation !=
              last_correlation.activation_generation ||
          request.correlation.connection_generation !=
              last_correlation.connection_generation))) {
      ok = false;
      break;
    }
    last_correlation = request.correlation;
    have_correlation = true;

    if (ui_only_endpoint && hello_complete &&
        request.command != Command::UpdateUiState) {
      Frame rejected;
      rejected.command = request.command;
      rejected.flags = kFlagResponse;
      rejected.status = Status::InvalidFrame;
      rejected.correlation = request.correlation;
      if (pipe_io::WriteFrame(
              pipe.get(), rejected,
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              error) != pipe_io::Result::Ok) {
        ok = false;
      }
      break;
    }

    const bool process_key = request.command == Command::ProcessKey;
    if (request.command == Command::AbandonConnection ||
        request.command == Command::AbandonSession)
      if (terminal_abandon_count)
        terminal_abandon_count->fetch_add(1);
    const bool execute_prepared =
        request.command == Command::ExecutePrepared;
    const bool open_session = request.command == Command::OpenSession;
    if (open_session)
      ++open_session_count;
    const bool ui_state = request.command == Command::UpdateUiState;
    if (request.command != Command::Hello && !ui_state)
      only_ui_state = false;
    const bool fault_target =
        fault == ServerFault::DisconnectBeforeExecute ||
                fault == ServerFault::DisconnectAfterDispatch ||
                IsCopyFailureFault(fault)
            ? execute_prepared
        : fault == ServerFault::UiHang
                                  ? ui_state
                              : fault == ServerFault::OpenSessionDelay ||
                                        fault == ServerFault::OpenSessionHang ||
                                        fault == ServerFault::OpenSessionUnavailable
                                  ? open_session
                                  : process_key;
    const bool inject =
        hello_complete && fault != ServerFault::None && fault_target &&
        !(IsCopyFailureFault(fault) && copy_failure_injected) &&
        (fault == ServerFault::OpenSessionUnavailable
             ? open_session_count <= std::max(1u, fault_after_process_keys)
             : process_key_count >= fault_after_process_keys);
    if (process_key)
      ++process_key_count;
    if (inject && (fault == ServerFault::Disconnect ||
                   fault == ServerFault::DisconnectBeforeExecute))
      break;
    if (inject && fault == ServerFault::MalformedReply) {
      Frame malformed = request;
      malformed.flags = kFlagResponse;
      std::vector<uint8_t> bytes;
      if (EncodeFrame(malformed, &bytes, error) && bytes.size() > kHeaderSize) {
        bytes.back() ^= 0xff;
        pipe_io::WriteBytes(
            pipe.get(), bytes,
            std::chrono::steady_clock::now() + std::chrono::seconds(1), error);
      }
      break;
    }
    if (inject && IsDelayedFault(fault)) {
      const auto delay = fault == ServerFault::OpenSessionDelay
                             ? std::chrono::milliseconds(100)
                         : fault == ServerFault::OpenSessionHang
                             ? kSessionOpenDeadline + std::chrono::milliseconds(100)
                             : std::chrono::milliseconds(250);
      Sleep(static_cast<DWORD>(delay.count()));
    }
    if (inject && fault == ServerFault::NoReply)
      break;

    Frame reply;
    if (inject && fault == ServerFault::OpenSessionUnavailable) {
      reply.command = request.command;
      reply.flags = kFlagResponse;
      reply.status = Status::Unavailable;
      reply.correlation = request.correlation;
    } else {
      if (inject && IsCopyFailureFault(fault)) {
        SetEnvironmentVariableA("FAMO_TEST_RUNTIME_COPY_FAILURE", "1");
        copy_failure_injected = true;
      }
      if constexpr (std::is_same_v<Service, RuntimeService>) {
        reply = service->DispatchForDelivery(request, client_identity);
      } else if constexpr (std::is_same_v<Service,
                                          RuntimeControlService>) {
        reply = service->Dispatch(request, client_identity);
      } else {
        reply = service->Dispatch(request);
      }
    }
    if (fault == ServerFault::CopyFailureAfterMutation &&
        request.command == Command::AbandonSession) {
      SetEnvironmentVariableA("FAMO_TEST_RUNTIME_COPY_FAILURE", nullptr);
    }
    if (inject && fault == ServerFault::DisconnectAfterDispatch)
      break;
    if (inject && fault == ServerFault::WrongVersionReply)
      reply.wire_version = kMinSupportedProtocolVersion;
    bool hello_accepted = false;
    if (request.command == Command::Hello) {
      hello_accepted =
          reply.flags == kFlagResponse && reply.command == Command::Hello &&
          reply.status == Status::Ok &&
          reply.correlation == request.correlation;
      // Dispatch may have inserted the logical client already. From this point
      // every write/encode/transport failure must invalidate that exact owner.
      authenticated = hello_accepted;
    }
    const pipe_io::Result written = pipe_io::WriteFrame(
        pipe.get(), reply,
        std::chrono::steady_clock::now() + std::chrono::seconds(1), error);
    if (written != pipe_io::Result::Ok) {
      if (inject)
        break;
      ok = false;
      break;
    }
    if (request.command == Command::Hello) {
      hello_complete = hello_accepted;
      if (!hello_accepted)
        break;
    }
    if (inject && fault != ServerFault::OpenSessionDelay &&
        fault != ServerFault::OpenSessionHang &&
        fault != ServerFault::OpenSessionUnavailable &&
        !IsCopyFailureFault(fault))
      break;
  }
  } catch (...) {
    ok = false;
  }
  if (IsCopyFailureFault(fault))
    SetEnvironmentVariableA("FAMO_TEST_RUNTIME_COPY_FAILURE", nullptr);
  if (authenticated && have_correlation &&
      (!preserve_ui_only_session || !only_ui_state)) {
    if constexpr (std::is_same_v<Service, RuntimeService>) {
      service->InvalidateConnection(
          last_correlation.client_id,
          last_correlation.activation_generation,
          last_correlation.connection_generation, client_identity);
    } else {
      service->InvalidateConnection(last_correlation.client_id,
                                    last_correlation.activation_generation,
                                    last_correlation.connection_generation,
                                    client_identity);
    }
  }
  DisconnectNamedPipe(pipe.get());
  return ok;
}

bool RuntimePipeServer::ServeOnce(const PipeEndpoint &endpoint,
                                  RuntimeService *service, ServerFault fault,
                                  std::chrono::milliseconds accept_timeout,
                                  std::string *error,
                                  uint32_t fault_after_process_keys,
                                  PipeServerStop *stop,
                                  bool ui_only_endpoint,
                                  std::atomic<uint32_t> *terminal_abandon_count) {
  try {
    return ServeOnceImpl(endpoint, service, fault, accept_timeout, error,
                         fault_after_process_keys, stop, true,
                         ui_only_endpoint, terminal_abandon_count);
  } catch (...) {
    try {
      if (error)
        *error = "runtime pipe server exception";
    } catch (...) {
    }
    return false;
  }
}

bool ControlPipeServer::ServeOnce(const PipeEndpoint &endpoint,
                                  RuntimeControlService *service,
                                  std::chrono::milliseconds accept_timeout,
                                  std::string *error, PipeServerStop *stop) {
  try {
    return ServeOnceImpl(endpoint, service, ServerFault::None, accept_timeout,
                         error, 0, stop, false, false, nullptr);
  } catch (...) {
    try {
      if (error)
        *error = "control pipe server exception";
    } catch (...) {
    }
    return false;
  }
}

bool PipeServerStop::Register(HANDLE pipe) {
  try {
    std::lock_guard lock(mutex_);
    if (stopped_)
      return false;
    pipes_.push_back(pipe);
    return true;
  } catch (...) {
    return false;
  }
}

void PipeServerStop::Unregister(HANDLE pipe) noexcept {
  try {
    std::lock_guard lock(mutex_);
    const auto found = std::find(pipes_.begin(), pipes_.end(), pipe);
    if (found != pipes_.end())
      pipes_.erase(found);
  } catch (...) {
  }
}

void PipeServerStop::Stop() noexcept {
  try {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (HANDLE pipe : pipes_) {
      CancelIoEx(pipe, nullptr);
      DisconnectNamedPipe(pipe);
    }
  } catch (...) {
  }
}

bool ParseServerFault(std::string_view value, ServerFault *fault) {
  if (!fault)
    return false;
  if (value == "none")
    *fault = ServerFault::None;
  else if (value == "no-read")
    *fault = ServerFault::NoRead;
  else if (value == "no-reply")
    *fault = ServerFault::NoReply;
  else if (value == "malformed")
    *fault = ServerFault::MalformedReply;
  else if (value == "wrong-version")
    *fault = ServerFault::WrongVersionReply;
  else if (value == "disconnect")
    *fault = ServerFault::Disconnect;
  else if (value == "engine-hang")
    *fault = ServerFault::EngineHang;
  else if (value == "open-session-delay")
    *fault = ServerFault::OpenSessionDelay;
  else if (value == "open-session-hang")
    *fault = ServerFault::OpenSessionHang;
  else if (value == "open-session-unavailable")
    *fault = ServerFault::OpenSessionUnavailable;
  else if (value == "ui-hang")
    *fault = ServerFault::UiHang;
  else if (value == "late")
    *fault = ServerFault::LateReply;
  else if (value == "disconnect-before-execute")
    *fault = ServerFault::DisconnectBeforeExecute;
  else if (value == "disconnect-after-dispatch")
    *fault = ServerFault::DisconnectAfterDispatch;
  else if (value == "copy-failure")
    *fault = ServerFault::CopyFailureAfterMutation;
  else if (value == "copy-failure-sticky")
    *fault = ServerFault::CopyFailureAfterMutationSticky;
  else
    return false;
  return true;
}

} // namespace famo::runtime
