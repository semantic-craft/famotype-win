#include "famo_runtime_pipe.h"
#include "famo_runtime_control.h"

#include <algorithm>
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

} // namespace

template <typename Service>
bool ServeOnceImpl(const PipeEndpoint &endpoint, Service *service,
                   ServerFault fault,
                   std::chrono::milliseconds accept_timeout,
                   std::string *error,
                   uint32_t fault_after_process_keys, PipeServerStop *stop,
                   bool preserve_ui_only_session) {
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
  if (!ConnectClient(pipe.get(), accept_timeout, error) ||
      !VerifyPipeClient(pipe.get(), endpoint, error)) {
    return false;
  }

  Correlation last_correlation{};
  bool have_correlation = false;
  bool hello_complete = false;
  bool only_ui_state = true;
  bool ok = true;
  uint32_t process_key_count = 0;
  uint32_t open_session_count = 0;
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
    last_correlation = request.correlation;
    have_correlation = true;

    const bool process_key = request.command == Command::ProcessKey;
    const bool open_session = request.command == Command::OpenSession;
    if (open_session)
      ++open_session_count;
    const bool ui_state = request.command == Command::UpdateUiState;
    if (request.command != Command::Hello && !ui_state)
      only_ui_state = false;
    const bool fault_target = fault == ServerFault::UiHang
                                  ? ui_state
                              : fault == ServerFault::OpenSessionDelay ||
                                        fault == ServerFault::OpenSessionHang ||
                                        fault == ServerFault::OpenSessionUnavailable
                                  ? open_session
                                  : process_key;
    const bool inject =
        hello_complete && fault != ServerFault::None && fault_target &&
        (fault == ServerFault::OpenSessionUnavailable
             ? open_session_count <= std::max(1u, fault_after_process_keys)
             : process_key_count >= fault_after_process_keys);
    if (process_key)
      ++process_key_count;
    if (inject && fault == ServerFault::Disconnect)
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
    if (inject && IsDelayedFault(fault))
      Sleep(fault == ServerFault::OpenSessionDelay ? 100 : 250);
    if (inject && fault == ServerFault::NoReply)
      break;

    Frame reply;
    if (inject && fault == ServerFault::OpenSessionUnavailable) {
      reply.command = request.command;
      reply.flags = kFlagResponse;
      reply.status = Status::Unavailable;
      reply.correlation = request.correlation;
    } else {
      reply = service->Dispatch(request);
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
    if (request.command == Command::Hello)
      hello_complete = true;
    if (inject && fault != ServerFault::OpenSessionDelay &&
        fault != ServerFault::OpenSessionHang &&
        fault != ServerFault::OpenSessionUnavailable)
      break;
  }
  if (have_correlation && (!preserve_ui_only_session || !only_ui_state)) {
    service->InvalidateConnection(last_correlation.client_id,
                                  last_correlation.activation_generation,
                                  last_correlation.connection_generation);
  }
  DisconnectNamedPipe(pipe.get());
  return ok;
}

bool RuntimePipeServer::ServeOnce(const PipeEndpoint &endpoint,
                                  RuntimeService *service, ServerFault fault,
                                  std::chrono::milliseconds accept_timeout,
                                  std::string *error,
                                  uint32_t fault_after_process_keys,
                                  PipeServerStop *stop) {
  return ServeOnceImpl(endpoint, service, fault, accept_timeout, error,
                       fault_after_process_keys, stop, true);
}

bool ControlPipeServer::ServeOnce(const PipeEndpoint &endpoint,
                                  RuntimeControlService *service,
                                  std::chrono::milliseconds accept_timeout,
                                  std::string *error, PipeServerStop *stop) {
  return ServeOnceImpl(endpoint, service, ServerFault::None, accept_timeout,
                       error, 0, stop, false);
}

bool PipeServerStop::Register(HANDLE pipe) {
  std::lock_guard lock(mutex_);
  if (stopped_)
    return false;
  pipes_.push_back(pipe);
  return true;
}

void PipeServerStop::Unregister(HANDLE pipe) {
  std::lock_guard lock(mutex_);
  const auto found = std::find(pipes_.begin(), pipes_.end(), pipe);
  if (found != pipes_.end())
    pipes_.erase(found);
}

void PipeServerStop::Stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  for (HANDLE pipe : pipes_) {
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
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
  else
    return false;
  return true;
}

} // namespace famo::runtime
