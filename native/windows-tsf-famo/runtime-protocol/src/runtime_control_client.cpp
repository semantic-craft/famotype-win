#include "famo_runtime_control.h"

#include <algorithm>

#include <windows.h>

#include "pipe_io.h"
#include "win_handle.h"

namespace famo::runtime {
namespace {

DWORD Remaining(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  return static_cast<DWORD>(std::max<int64_t>(
      1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
             .count()));
}

bool ValidReply(const Frame &request, const Frame &reply) {
  return reply.flags == kFlagResponse && reply.command == request.command &&
         reply.correlation == request.correlation;
}

bool Exchange(HANDLE pipe, const Frame &request, Frame *reply,
              std::chrono::steady_clock::time_point deadline,
              std::string *error) {
  return pipe_io::WriteFrame(pipe, request, deadline, error) ==
             pipe_io::Result::Ok &&
         pipe_io::ReadFrame(pipe, reply, deadline, error) ==
             pipe_io::Result::Ok &&
         ValidReply(request, *reply);
}

HANDLE Connect(const PipeEndpoint &endpoint, std::wstring_view expected_server,
               std::chrono::steady_clock::time_point deadline,
               std::string *error) {
  while (std::chrono::steady_clock::now() < deadline) {
    HANDLE pipe = CreateFileW(endpoint.name.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      if (VerifyPipeServer(pipe, endpoint, expected_server, error))
        return pipe;
      CloseHandle(pipe);
      return INVALID_HANDLE_VALUE;
    }
    const DWORD last_error = GetLastError();
    if (last_error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(endpoint.name.c_str(), Remaining(deadline));
    } else if (last_error == ERROR_FILE_NOT_FOUND) {
      Sleep(std::min<DWORD>(5, Remaining(deadline)));
    } else {
      if (error)
        *error = "control pipe open failed: " + std::to_string(last_error);
      return INVALID_HANDLE_VALUE;
    }
  }
  if (error)
    *error = "control pipe connect deadline elapsed";
  return INVALID_HANDLE_VALUE;
}

uint64_t ClientId() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  uint64_t value = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                   static_cast<uint64_t>(counter.QuadPart);
  return value == 0 ? 1 : value;
}

} // namespace

bool RunControlClient(const PipeEndpoint &endpoint,
                      std::wstring_view expected_server, Command command,
                      std::chrono::milliseconds timeout, ControlResult *result,
                      std::string *error) {
  if (!result || !IsControlOperation(command) || timeout.count() <= 0) {
    if (error)
      *error = "invalid control client arguments";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto connect_deadline =
      std::min(deadline, std::chrono::steady_clock::now() +
                            std::chrono::seconds(2));
  win::UniqueHandle pipe(
      Connect(endpoint, expected_server, connect_deadline, error));
  if (!pipe)
    return false;
  DWORD mode = PIPE_READMODE_BYTE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
    if (error)
      *error = "control pipe mode failed";
    return false;
  }

  Correlation correlation{ClientId(), 1, 1, 0, 0, 0};
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = correlation;
  Frame reply;
  if (!Exchange(pipe.get(), hello, &reply, deadline, error) ||
      reply.status != Status::Ok) {
    if (error && error->empty())
      *error = "control Hello failed";
    return false;
  }

  Frame request;
  request.command = command;
  request.correlation = correlation;
  request.correlation.sequence = 1;
  if (!Exchange(pipe.get(), request, &reply, deadline, error) ||
      reply.status != Status::Ok ||
      !DecodeControlResult(reply.payload, result, error))
    return false;

  uint64_t sequence = 2;
  while (result->state == ControlState::Pending ||
         result->state == ControlState::Running) {
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error)
        *error = "control operation deadline elapsed";
      return false;
    }
    Sleep(std::min<DWORD>(10, Remaining(deadline)));
    Frame status;
    status.command = Command::ControlStatus;
    status.correlation = correlation;
    status.correlation.sequence = sequence++;
    if (!EncodeControlOperationId(result->operation_id, &status.payload) ||
        !Exchange(pipe.get(), status, &reply, deadline, error) ||
        reply.status != Status::Ok ||
        !DecodeControlResult(reply.payload, result, error))
      return false;
  }
  return true;
}

} // namespace famo::runtime
