#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "famo_pipe_security.h"
#include "famo_runtime_service.h"
#include "famo_runtime_state.h"

namespace famo::runtime {

namespace pipe_io {
class RetirementGate;
}

class PipeServerStop {
public:
  bool Register(HANDLE pipe);
  void Unregister(HANDLE pipe);
  void Stop();

private:
  std::mutex mutex_;
  std::vector<HANDLE> pipes_;
  bool stopped_ = false;
};

constexpr std::chrono::milliseconds kHardCallDeadline{50};
// Session creation is an activation/control-path operation and may perform the
// first librime context allocation. It remains strictly bounded, but does not
// consume the tighter per-key callback budget.
constexpr std::chrono::milliseconds kSessionOpenDeadline{1000};

enum class ServerFault {
  None,
  NoRead,
  NoReply,
  MalformedReply,
  Disconnect,
  EngineHang,
  OpenSessionDelay,
  OpenSessionHang,
  OpenSessionUnavailable,
  UiHang,
  LateReply,
};

class PipeRuntimePort {
public:
  PipeRuntimePort();
  ~PipeRuntimePort();
  PipeRuntimePort(const PipeRuntimePort &) = delete;
  PipeRuntimePort &operator=(const PipeRuntimePort &) = delete;

  // Control path only. This may wait for a server and inspect its process.
  bool Connect(const PipeEndpoint &endpoint, std::wstring_view expected_server,
               const Correlation &connection_identity,
               std::chrono::milliseconds timeout, std::string *error,
               const std::atomic<bool> *cancelled = nullptr);
  void CancelConnect();
  void CancelCall();
  // Control path only. It may cancel I/O and join the worker.
  void Stop();

  // Hot path. It only enqueues to an already-ready worker and waits until one
  // absolute deadline. It never connects, launches, cancels, or joins.
  CallResult Call(Frame &&request, std::chrono::milliseconds deadline);
  // Hot path. Overwrites the previous pending UI-only update without doing
  // pipe I/O. Best-effort UI work uses an independent pipe and worker.
  void Post(Frame &&request);
  // Hot path. Atomically rejects later calls after host-side payload or apply
  // validation fails; cleanup remains owned by the control path/worker.
  void Poison();

  ChannelState state() const;
  uint64_t connection_generation() const;

private:
  struct WorkItem {
    Frame request;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
    std::condition_variable completed;
    CallResult result;
    bool done = false;
  };

  void WorkerMain();
  void UiWorkerMain();
  void OpenCircuit();

  mutable std::timed_mutex mutex_;
  std::mutex connect_mutex_;
  std::condition_variable_any available_;
  WorkItem work_;
  std::thread worker_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE connecting_pipe_ = INVALID_HANDLE_VALUE;
  std::atomic<ChannelState> state_{ChannelState::NotReady};
  std::atomic<uint64_t> connection_generation_{0};
  std::atomic<bool> slot_busy_{false};
  std::shared_ptr<pipe_io::RetirementGate> retirement_;
  bool stop_ = false;
  bool queued_ = false;
  bool in_flight_ = false;

  std::mutex ui_mutex_;
  std::thread ui_worker_;
  HANDLE ui_pipe_ = INVALID_HANDLE_VALUE;
  std::shared_ptr<pipe_io::RetirementGate> ui_retirement_;
  std::atomic<bool> ui_stop_{false};
  std::atomic<bool> ui_ready_{false};
  std::atomic<uint64_t> ui_wake_epoch_{0};
  std::atomic<std::shared_ptr<const Frame>> posted_request_;
};

class RuntimePipeServer {
public:
  bool ServeOnce(const PipeEndpoint &endpoint, RuntimeService *service,
                 ServerFault fault, std::chrono::milliseconds accept_timeout,
                 std::string *error, uint32_t fault_after_process_keys = 0,
                 PipeServerStop *stop = nullptr);
};

class RuntimeControlService;

class ControlPipeServer {
public:
  bool ServeOnce(const PipeEndpoint &endpoint, RuntimeControlService *service,
                 std::chrono::milliseconds accept_timeout,
                 std::string *error, PipeServerStop *stop = nullptr);
};

bool ParseServerFault(std::string_view value, ServerFault *fault);

} // namespace famo::runtime
