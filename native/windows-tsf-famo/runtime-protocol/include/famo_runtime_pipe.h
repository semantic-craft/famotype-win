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
  void Unregister(HANDLE pipe) noexcept;
  void Stop() noexcept;

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
  WrongVersionReply,
  Disconnect,
  EngineHang,
  OpenSessionDelay,
  OpenSessionHang,
  OpenSessionUnavailable,
  UiHang,
  LateReply,
  DisconnectBeforeExecute,
  DisconnectAfterDispatch,
  CopyFailureAfterMutation,
  CopyFailureAfterMutationSticky,
};

struct DeliveryResult {
  // Status of the delivery-control operation. Ok means final_reply is the
  // exact cached business response; its own status remains authoritative.
  Status status = Status::Unavailable;
  Frame final_reply;
  std::chrono::milliseconds elapsed{0};
};

class PipeRuntimePort {
public:
  explicit PipeRuntimePort(uint32_t bridge_abi = 0);
  ~PipeRuntimePort();
  PipeRuntimePort(const PipeRuntimePort &) = delete;
  PipeRuntimePort &operator=(const PipeRuntimePort &) = delete;

  // Control path only. This may wait for a server and inspect its process.
  bool Connect(const PipeEndpoint &endpoint, std::wstring_view expected_server,
               const Correlation &connection_identity,
               std::chrono::milliseconds timeout, std::string *error,
               const std::atomic<bool> *cancelled = nullptr);
  void CancelConnect();
  void CancelCall(bool preserve_connection_generation = false);
  // Control path only. It may cancel I/O and join the worker.
  void Stop() noexcept;

  // Hot path. It only enqueues to an already-ready worker and waits until one
  // absolute deadline. It never connects, launches, cancels, or joins.
  CallResult Call(Frame &&request, std::chrono::milliseconds deadline);
  CallResult Call(Frame &&request,
                  std::chrono::steady_clock::time_point absolute_deadline);
  // Delivery-tracked business calls use one caller-owned absolute deadline for
  // both Prepare and Execute. Claim/ACK may be issued later by recovery work.
  CallResult Prepare(Frame &&request,
                     std::chrono::steady_clock::time_point absolute_deadline);
  DeliveryResult
  ExecutePrepared(const DeliveryReference &reference,
                  std::chrono::steady_clock::time_point absolute_deadline);
  DeliveryResult
  Claim(const DeliveryReference &reference,
        std::chrono::steady_clock::time_point absolute_deadline);
  CallResult Ack(const DeliveryReference &reference,
                 std::chrono::steady_clock::time_point absolute_deadline);
  CallResult
  AbandonSession(const DeliveryReference &reference,
                 std::chrono::steady_clock::time_point absolute_deadline);
  // Hot path. Overwrites the previous pending UI-only update without doing
  // pipe I/O. Best-effort UI work uses an independent pipe and worker.
  void Post(Frame &&request);
  // Hot path. Atomically rejects later calls after host-side payload or apply
  // validation fails; cleanup remains owned by the control path/worker.
  void Poison();

  ChannelState state() const;
  uint64_t connection_generation() const;
  // Exact process identity authenticated during the primary pipe handshake.
  // A zero identity means the channel is not currently authenticated.
  PipeClientIdentity server_identity() const noexcept;

private:
  struct WorkItem {
    Frame request;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
    std::condition_variable completed;
    CallResult result;
    bool done = false;
  };

  void WorkerMain() noexcept;
  void UiWorkerMain() noexcept;
  HANDLE ConnectPipeChannel(
      const PipeEndpoint &endpoint, std::wstring_view expected_server,
      const Correlation &connection_identity,
      std::chrono::steady_clock::time_point deadline, std::string *error,
      const std::shared_ptr<pipe_io::RetirementGate> &retirement,
      std::mutex &connect_mutex, HANDLE *connecting_pipe,
      const std::atomic<bool> *cancelled,
      PipeClientIdentity *server_identity,
      uint16_t *protocol_version);
  void QueueUiRequest(std::shared_ptr<const Frame> request) noexcept;
  void RequeueUiRequest(std::shared_ptr<const Frame> request) noexcept;
  void OpenCircuit(bool preserve_connection_generation = false);
  CallResult CallUntil(Frame &&request,
                       std::chrono::steady_clock::time_point absolute_deadline);
  Frame DeliveryControl(Command command,
                        const DeliveryReference &reference) const;
  DeliveryResult DeliveryCall(
      Command command, const DeliveryReference &reference,
      std::chrono::steady_clock::time_point absolute_deadline);

  mutable std::timed_mutex mutex_;
  std::mutex connect_mutex_;
  std::condition_variable_any available_;
  WorkItem work_;
  std::thread worker_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE connecting_pipe_ = INVALID_HANDLE_VALUE;
  std::atomic<ChannelState> state_{ChannelState::NotReady};
  std::atomic<uint64_t> connection_generation_{0};
  std::atomic<uint64_t> server_creation_time_{0};
  std::atomic<uint32_t> server_process_id_{0};
  std::atomic<uint16_t> protocol_version_{kProtocolVersion};
  std::atomic<std::shared_ptr<const Correlation>> connection_identity_;
  std::atomic<bool> slot_busy_{false};
  std::shared_ptr<pipe_io::RetirementGate> retirement_;
  bool stop_ = false;
  bool queued_ = false;
  bool in_flight_ = false;
  const uint32_t bridge_abi_ = 0;

  std::mutex ui_mutex_;
  std::thread ui_worker_;
  HANDLE ui_pipe_ = INVALID_HANDLE_VALUE;
  HANDLE ui_connecting_pipe_ = INVALID_HANDLE_VALUE;
  std::shared_ptr<pipe_io::RetirementGate> ui_retirement_;
  PipeEndpoint ui_endpoint_;
  std::wstring ui_expected_server_;
  Correlation ui_connection_identity_;
  PipeClientIdentity ui_expected_identity_;
  uint16_t ui_expected_protocol_ = kProtocolVersion;
  std::atomic<bool> ui_stop_{false};
  std::atomic<uint64_t> ui_wake_epoch_{0};
  std::atomic<std::shared_ptr<const Frame>> posted_request_;
};

class RuntimePipeServer {
public:
  bool ServeOnce(const PipeEndpoint &endpoint, RuntimeService *service,
                 ServerFault fault, std::chrono::milliseconds accept_timeout,
                 std::string *error, uint32_t fault_after_process_keys = 0,
                 PipeServerStop *stop = nullptr,
                 bool ui_only_endpoint = false,
                 std::atomic<uint32_t> *terminal_abandon_count = nullptr);
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
