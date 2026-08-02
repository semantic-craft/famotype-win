#include "famo_runtime_pipe.h"

#include <algorithm>

#include "pipe_io.h"

namespace famo::runtime {
namespace {

// A Windows wait can resume one default scheduler quantum after its nominal
// timeout. Keep that delay inside the caller-visible hard deadline instead of
// adding it after the deadline on the TSF host thread.
constexpr std::chrono::milliseconds kWaitSchedulingReserve{16};
constexpr std::chrono::milliseconds kOptionalUiConnectBudget{25};
constexpr std::chrono::milliseconds kUiReconnectInitialBackoff{10};
constexpr std::chrono::milliseconds kUiReconnectMaximumBackoff{1000};

DWORD Remaining(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  const auto value =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<DWORD>(std::max<int64_t>(1, value.count()));
}

bool ValidReply(const Frame &request, const Frame &reply) {
  if (reply.flags != kFlagResponse)
    return false;
  if (request.command != Command::Hello &&
      reply.wire_version != request.wire_version)
    return false;
  if (reply.command == request.command &&
      reply.correlation == request.correlation) {
    return true;
  }
  if (request.command != Command::ExecutePrepared &&
      request.command != Command::ClaimResult) {
    return false;
  }
  DeliveryReference reference;
  std::string error;
  return DecodeDeliveryReference(request.payload, &reference, &error) &&
         reply.command == reference.command &&
         reply.correlation == reference.correlation;
}

bool SameSession(const Correlation &left, const Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation &&
         left.session_id == right.session_id &&
         left.session_generation == right.session_generation;
}

bool PreservesLogicalConnection(Command command) {
  return IsDeliveryTracked(command) ||
         command == Command::ExecutePrepared ||
         command == Command::ClaimResult || command == Command::AckResult ||
         command == Command::AbandonConnection ||
         command == Command::AbandonSession;
}

std::chrono::milliseconds MaximumCallDeadline(Command command) {
  if (command == Command::OpenSession)
    return kSessionOpenDeadline;
  if (command == Command::SearchCandidates)
    return kSearchCandidatesDeadline;
  return kHardCallDeadline;
}

} // namespace

PipeRuntimePort::PipeRuntimePort(uint32_t bridge_abi)
    : retirement_(pipe_io::MakeRetirementGate()),
      bridge_abi_(bridge_abi),
      ui_retirement_(pipe_io::MakeRetirementGate()) {}

PipeRuntimePort::~PipeRuntimePort() { Stop(); }

HANDLE PipeRuntimePort::ConnectPipeChannel(
    const PipeEndpoint &endpoint, std::wstring_view expected_server,
    const Correlation &connection_identity,
    std::chrono::steady_clock::time_point deadline, std::string *error,
    const std::shared_ptr<pipe_io::RetirementGate> &retirement,
    std::mutex &connect_mutex, HANDLE *connecting_pipe,
    const std::atomic<bool> *cancelled, PipeClientIdentity *server_identity,
    uint16_t *protocol_version) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (pipe == INVALID_HANDLE_VALUE &&
         std::chrono::steady_clock::now() < deadline &&
         (!cancelled || !cancelled->load())) {
    HANDLE candidate =
        CreateFileW(endpoint.name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (candidate != INVALID_HANDLE_VALUE) {
      {
        std::lock_guard lock(connect_mutex);
        *connecting_pipe = candidate;
      }
      const auto release_candidate = [&](bool close) {
        {
          std::lock_guard lock(connect_mutex);
          if (*connecting_pipe == candidate)
            *connecting_pipe = INVALID_HANDLE_VALUE;
        }
        if (close)
          CloseHandle(candidate);
      };
      if (cancelled && cancelled->load()) {
        release_candidate(true);
        break;
      }
      PipeClientIdentity candidate_identity;
      if (!VerifyPipeServer(candidate, endpoint, expected_server, error,
                            &candidate_identity)) {
        release_candidate(true);
        return INVALID_HANDLE_VALUE;
      }
      DWORD mode = PIPE_READMODE_BYTE;
      if (!SetNamedPipeHandleState(candidate, &mode, nullptr, nullptr)) {
        if (error) {
          *error = "SetNamedPipeHandleState failed: " +
                   std::to_string(GetLastError());
        }
        release_candidate(true);
        return INVALID_HANDLE_VALUE;
      }

      Frame hello;
      hello.command = Command::Hello;
      hello.correlation = connection_identity;
      if (bridge_abi_ != 0) {
        const HelloRequest offer{kMinSupportedProtocolVersion, kProtocolVersion,
                                 bridge_abi_};
        if (!EncodeHelloRequest(offer, &hello.payload, error)) {
          release_candidate(true);
          return INVALID_HANDLE_VALUE;
        }
      }
      Frame hello_reply;
      const pipe_io::Result hello_write = pipe_io::WriteFrame(
          candidate, hello, deadline, error, retirement);
      const pipe_io::Result hello_read =
          hello_write == pipe_io::Result::Ok
              ? pipe_io::ReadFrame(candidate, &hello_reply, deadline, error,
                                   retirement)
              : hello_write;
      bool negotiated =
          hello_read == pipe_io::Result::Ok && ValidReply(hello, hello_reply) &&
          hello_reply.status == Status::Ok;
      uint16_t selected_protocol = hello_reply.wire_version;
      if (negotiated && bridge_abi_ != 0) {
        HelloResponse response;
        negotiated =
            DecodeHelloResponse(hello_reply.payload, &response, error) &&
            response.selected_protocol_version == hello_reply.wire_version &&
            response.selected_protocol_version >=
                kMinSupportedProtocolVersion &&
            response.selected_protocol_version <= kProtocolVersion;
        selected_protocol = response.selected_protocol_version;
      } else if (negotiated) {
        negotiated = hello_reply.payload.empty();
      }
      if (negotiated && (!cancelled || !cancelled->load())) {
        if (server_identity)
          *server_identity = candidate_identity;
        if (protocol_version)
          *protocol_version = selected_protocol;
        pipe = candidate;
        release_candidate(false);
        break;
      }
      const bool retryable =
          hello_read == pipe_io::Result::Ok && ValidReply(hello, hello_reply) &&
          hello_reply.status == Status::Unavailable;
      release_candidate(true);
      if (cancelled && cancelled->load())
        break;
      if (!retryable) {
        if (error && error->empty())
          *error = "pipe Hello handshake failed";
        return INVALID_HANDLE_VALUE;
      }
      if (error)
        error->clear();
      Sleep(std::min<DWORD>(2, Remaining(deadline)));
      continue;
    }
    const DWORD open_error = GetLastError();
    if (open_error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(endpoint.name.c_str(),
                     std::min<DWORD>(10, Remaining(deadline)));
    } else if (open_error == ERROR_FILE_NOT_FOUND) {
      Sleep(std::min<DWORD>(2, Remaining(deadline)));
    } else {
      if (error) {
        *error = "CreateFile(pipe) failed: " + std::to_string(open_error);
      }
      return INVALID_HANDLE_VALUE;
    }
  }
  if (pipe == INVALID_HANDLE_VALUE && error)
    *error = "pipe connect deadline elapsed";
  return pipe;
}

bool PipeRuntimePort::Connect(const PipeEndpoint &endpoint,
                              std::wstring_view expected_server,
                              const Correlation &connection_identity,
                              std::chrono::milliseconds timeout,
                              std::string *error,
                              const std::atomic<bool> *cancelled) {
  Stop();
  if (!pipe_io::RetirementReady(retirement_)) {
    if (error)
      *error = "previous pipe I/O is still retiring";
    return false;
  }
  if (connection_identity.client_id == 0 ||
      connection_identity.activation_generation == 0 ||
      connection_identity.connection_generation == 0 ||
      connection_identity.session_id != 0 ||
      connection_identity.session_generation != 0 ||
      connection_identity.sequence != 0 || timeout.count() <= 0) {
    if (error)
      *error = "invalid connection arguments";
    return false;
  }
  std::shared_ptr<const Correlation> connected_identity;
  try {
    connected_identity =
        std::make_shared<const Correlation>(connection_identity);
  } catch (...) {
    if (error)
      *error = "connection identity allocation failed";
    return false;
  }
  PipeEndpoint ui_endpoint;
  std::wstring ui_expected_server;
  std::shared_ptr<pipe_io::RetirementGate> next_ui_retirement;
  try {
    ui_endpoint = BuildUiPipeEndpoint(endpoint);
    ui_expected_server.assign(expected_server);
    next_ui_retirement = pipe_io::MakeRetirementGate();
  } catch (...) {
    if (error)
      *error = "UI recovery state allocation failed";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  PipeClientIdentity primary_server;
  uint16_t primary_protocol = 0;
  HANDLE pipe = ConnectPipeChannel(
      endpoint, expected_server, connection_identity, deadline, error,
      retirement_, connect_mutex_, &connecting_pipe_, cancelled,
      &primary_server, &primary_protocol);
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  // A cancelled best-effort transfer may still be retiring on its old gate.
  // The reaper retains that gate, so a reconnect can start a fresh optional UI
  // lane without weakening the primary lane's fail-closed retirement rule.
  std::string ignored_ui_error;
  const auto ui_deadline =
      std::min(deadline, std::chrono::steady_clock::now() +
                             kOptionalUiConnectBudget);
  PipeClientIdentity ui_server;
  uint16_t ui_protocol = 0;
  HANDLE ui_pipe = ConnectPipeChannel(
      ui_endpoint, expected_server, connection_identity, ui_deadline,
      &ignored_ui_error, next_ui_retirement, connect_mutex_,
      &connecting_pipe_, cancelled, &ui_server, &ui_protocol);
  if (ui_pipe != INVALID_HANDLE_VALUE &&
      (ui_server != primary_server || ui_protocol != primary_protocol)) {
    CloseHandle(ui_pipe);
    ui_pipe = INVALID_HANDLE_VALUE;
  }
  {
    std::lock_guard lock(mutex_);
    pipe_ = pipe;
    connection_generation_.store(connection_identity.connection_generation);
    // Publish creation time before PID; readers treat PID zero as invalid.
    server_creation_time_.store(primary_server.process_creation_time,
                                std::memory_order_release);
    server_process_id_.store(primary_server.process_id,
                             std::memory_order_release);
    protocol_version_.store(primary_protocol, std::memory_order_release);
    connection_identity_.store(std::move(connected_identity));
    state_.store(ChannelState::Ready);
    slot_busy_.store(false);
    stop_ = false;
    queued_ = false;
    in_flight_ = false;
  }
  {
    std::lock_guard lock(ui_mutex_);
    ui_pipe_ = ui_pipe;
    ui_connecting_pipe_ = INVALID_HANDLE_VALUE;
    ui_retirement_ = std::move(next_ui_retirement);
    ui_endpoint_ = std::move(ui_endpoint);
    ui_expected_server_ = std::move(ui_expected_server);
    ui_connection_identity_ = connection_identity;
    ui_expected_identity_ = primary_server;
    ui_expected_protocol_ = primary_protocol;
    ui_stop_ = false;
    posted_request_.store(nullptr);
  }
  try {
    worker_ = std::thread(&PipeRuntimePort::WorkerMain, this);
  } catch (...) {
    std::lock_guard lock(mutex_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    state_.store(ChannelState::NotReady);
    server_process_id_.store(0, std::memory_order_release);
    server_creation_time_.store(0, std::memory_order_release);
    protocol_version_.store(kProtocolVersion, std::memory_order_release);
    connection_identity_.store(nullptr);
    slot_busy_.store(false);
    if (ui_pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(ui_pipe);
      std::lock_guard ui_lock(ui_mutex_);
      ui_pipe_ = INVALID_HANDLE_VALUE;
    }
    if (error)
      *error = "pipe worker creation failed";
    return false;
  }
  try {
    ui_worker_ = std::thread(&PipeRuntimePort::UiWorkerMain, this);
  } catch (...) {
    std::lock_guard lock(ui_mutex_);
    if (ui_pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(ui_pipe_);
      ui_pipe_ = INVALID_HANDLE_VALUE;
    }
  }
  if (error)
    error->clear();
  return true;
}

void PipeRuntimePort::CancelConnect() {
  std::lock_guard lock(connect_mutex_);
  if (connecting_pipe_ != INVALID_HANDLE_VALUE)
    CancelIoEx(connecting_pipe_, nullptr);
}

void PipeRuntimePort::CancelCall(bool preserve_connection_generation) {
  OpenCircuit(preserve_connection_generation);
  std::lock_guard lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE)
    CancelIoEx(pipe_, nullptr);
}

void PipeRuntimePort::Stop() noexcept {
  try {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    pipe = pipe_;
  }
  {
    std::lock_guard lock(ui_mutex_);
    ui_stop_ = true;
    if (ui_pipe_ != INVALID_HANDLE_VALUE)
      CancelIoEx(ui_pipe_, nullptr);
    if (ui_connecting_pipe_ != INVALID_HANDLE_VALUE)
      CancelIoEx(ui_connecting_pipe_, nullptr);
  }
  available_.notify_all();
  ui_wake_epoch_.fetch_add(1);
  ui_wake_epoch_.notify_all();
  if (pipe != INVALID_HANDLE_VALUE)
    CancelIoEx(pipe, nullptr);
  if (worker_.joinable())
    worker_.join();
  if (ui_worker_.joinable())
    ui_worker_.join();

  {
    std::lock_guard lock(mutex_);
    if (queued_) {
      {
        std::lock_guard item_lock(work_.mutex);
        work_.result.status = Status::Unavailable;
        work_.done = true;
      }
      work_.completed.notify_all();
    }
    queued_ = false;
    if (pipe_ != INVALID_HANDLE_VALUE)
      CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    state_.store(ChannelState::NotReady);
    server_process_id_.store(0, std::memory_order_release);
    server_creation_time_.store(0, std::memory_order_release);
    protocol_version_.store(kProtocolVersion, std::memory_order_release);
    connection_identity_.store(nullptr);
    slot_busy_.store(false);
    stop_ = false;
    in_flight_ = false;
  }
  {
    std::lock_guard lock(ui_mutex_);
    posted_request_.store(nullptr);
    if (ui_pipe_ != INVALID_HANDLE_VALUE)
      CloseHandle(ui_pipe_);
    ui_pipe_ = INVALID_HANDLE_VALUE;
    ui_connecting_pipe_ = INVALID_HANDLE_VALUE;
    ui_stop_ = false;
  }
  } catch (...) {
    try {
      if (worker_.joinable())
        worker_.detach();
      if (ui_worker_.joinable())
        ui_worker_.detach();
    } catch (...) {
    }
    state_.store(ChannelState::NotReady);
    server_process_id_.store(0, std::memory_order_release);
    server_creation_time_.store(0, std::memory_order_release);
    slot_busy_.store(false);
  }
}

CallResult PipeRuntimePort::Call(Frame &&request,
                                 std::chrono::milliseconds deadline) {
  const auto started = std::chrono::steady_clock::now();
  const std::chrono::milliseconds maximum =
      MaximumCallDeadline(request.command);
  deadline = std::min(deadline, maximum);
  if (deadline.count() <= 0)
    return {};
  return CallUntil(std::move(request), started + deadline);
}

CallResult
PipeRuntimePort::Call(Frame &&request,
                      std::chrono::steady_clock::time_point absolute_deadline) {
  return CallUntil(std::move(request), absolute_deadline);
}

CallResult PipeRuntimePort::CallUntil(
    Frame &&request,
    std::chrono::steady_clock::time_point absolute_deadline) {
  const auto started = std::chrono::steady_clock::now();
  const bool preserve_connection_generation =
      PreservesLogicalConnection(request.command);
  CallResult immediate;
  const std::chrono::milliseconds maximum =
      MaximumCallDeadline(request.command);
  absolute_deadline =
      std::min(absolute_deadline, started + maximum);
  if (absolute_deadline <= started + std::chrono::milliseconds(1)) {
    immediate.status = Status::Timeout;
    return immediate;
  }
  const auto remaining = absolute_deadline - started;
  const auto reserve =
      std::min(remaining - std::chrono::milliseconds(1),
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   kWaitSchedulingReserve));
  const auto wait_deadline = absolute_deadline - reserve;
  if (state_.load() != ChannelState::Ready || slot_busy_.load()) {
    immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return immediate;
  }
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_until(wait_deadline)) {
    if (state_.load() == ChannelState::Ready) {
      OpenCircuit(preserve_connection_generation);
      immediate.status = Status::Timeout;
    }
    immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return immediate;
  }
  if (state_.load() != ChannelState::Ready || slot_busy_.load() || stop_ ||
      queued_ ||
      request.correlation.connection_generation !=
          connection_generation_.load()) {
    immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return immediate;
  }
  request.wire_version = protocol_version_.load(std::memory_order_acquire);
  work_.request = std::move(request);
  work_.deadline = wait_deadline;
  work_.result = {};
  work_.done = false;
  slot_busy_.store(true);
  queued_ = true;
  lock.unlock();
  available_.notify_one();

  std::unique_lock item_lock(work_.mutex);
  const bool done = work_.completed.wait_until(item_lock, wait_deadline,
                                               [&] { return work_.done; });
  const auto completed_at = std::chrono::steady_clock::now();
  if (done && completed_at < wait_deadline) {
    work_.result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(completed_at -
                                                              started);
    CallResult result = std::move(work_.result);
    item_lock.unlock();
    slot_busy_.store(false);
    return result;
  }
  item_lock.unlock();
  OpenCircuit(preserve_connection_generation);
  if (done)
    slot_busy_.store(false);
  immediate.status = Status::Timeout;
  immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      completed_at - started);
  return immediate;
}

CallResult PipeRuntimePort::Prepare(
    Frame &&request,
    std::chrono::steady_clock::time_point absolute_deadline) {
  if (!IsDeliveryTracked(request.command)) {
    CallResult invalid;
    invalid.status = Status::InvalidFrame;
    return invalid;
  }
  return CallUntil(std::move(request), absolute_deadline);
}

Frame PipeRuntimePort::DeliveryControl(
    Command command, const DeliveryReference &reference) const {
  Frame request;
  request.command = command;
  const std::shared_ptr<const Correlation> identity =
      connection_identity_.load();
  if (identity)
    request.correlation = *identity;
  (void)EncodeDeliveryReference(reference, &request.payload);
  return request;
}

DeliveryResult PipeRuntimePort::DeliveryCall(
    Command command, const DeliveryReference &reference,
    std::chrono::steady_clock::time_point absolute_deadline) {
  DeliveryResult delivery;
  Frame request = DeliveryControl(command, reference);
  if (request.correlation.client_id == 0 || request.payload.empty())
    return delivery;
  CallResult result = CallUntil(std::move(request), absolute_deadline);
  delivery.elapsed = result.elapsed;
  if (result.reply.flags == kFlagResponse &&
      result.reply.command == reference.command &&
      result.reply.correlation == reference.correlation) {
    if (result.reply.status == Status::RecoveryPending ||
        result.reply.status == Status::DeliveryFailed) {
      delivery.status = result.reply.status;
    } else {
      delivery.status = Status::Ok;
      delivery.final_reply = std::move(result.reply);
    }
  } else {
    delivery.status = result.status;
  }
  return delivery;
}

DeliveryResult PipeRuntimePort::ExecutePrepared(
    const DeliveryReference &reference,
    std::chrono::steady_clock::time_point absolute_deadline) {
  return DeliveryCall(Command::ExecutePrepared, reference, absolute_deadline);
}

DeliveryResult PipeRuntimePort::Claim(
    const DeliveryReference &reference,
    std::chrono::steady_clock::time_point absolute_deadline) {
  return DeliveryCall(Command::ClaimResult, reference, absolute_deadline);
}

CallResult PipeRuntimePort::Ack(
    const DeliveryReference &reference,
    std::chrono::steady_clock::time_point absolute_deadline) {
  Frame request = DeliveryControl(Command::AckResult, reference);
  if (request.correlation.client_id == 0 || request.payload.empty()) {
    CallResult invalid;
    invalid.status = Status::Unavailable;
    return invalid;
  }
  return CallUntil(std::move(request), absolute_deadline);
}

CallResult PipeRuntimePort::AbandonSession(
    const DeliveryReference &reference,
    std::chrono::steady_clock::time_point absolute_deadline) {
  Frame request = DeliveryControl(Command::AbandonSession, reference);
  if (request.correlation.client_id == 0 || request.payload.empty()) {
    CallResult invalid;
    invalid.status = Status::Unavailable;
    return invalid;
  }
  return CallUntil(std::move(request), absolute_deadline);
}

void PipeRuntimePort::QueueUiRequest(
    std::shared_ptr<const Frame> request) noexcept {
  if (!request)
    return;
  std::shared_ptr<const Frame> pending = posted_request_.load();
  for (;;) {
    if (pending &&
        SameSession(pending->correlation, request->correlation) &&
        request->correlation.sequence <= pending->correlation.sequence) {
      return;
    }
    if (posted_request_.compare_exchange_weak(pending, request)) {
      ui_wake_epoch_.fetch_add(1);
      ui_wake_epoch_.notify_one();
      return;
    }
  }
}

void PipeRuntimePort::RequeueUiRequest(
    std::shared_ptr<const Frame> request) noexcept {
  if (!request)
    return;
  std::shared_ptr<const Frame> empty;
  if (posted_request_.compare_exchange_strong(empty, std::move(request))) {
    ui_wake_epoch_.fetch_add(1);
    ui_wake_epoch_.notify_one();
  }
}

void PipeRuntimePort::Post(Frame &&request) {
  if (state_.load() != ChannelState::Ready ||
      request.command != Command::UpdateUiState ||
      request.correlation.connection_generation !=
          connection_generation_.load() ||
      ui_stop_.load()) {
    return;
  }
  request.wire_version = protocol_version_.load(std::memory_order_acquire);
  try {
    QueueUiRequest(std::make_shared<const Frame>(std::move(request)));
  } catch (...) {
    // UiState is best-effort; allocation failure must not escape into TSF.
  }
}

void PipeRuntimePort::Poison() { OpenCircuit(); }

void PipeRuntimePort::WorkerMain() noexcept {
  try {
  for (;;) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    Frame request;
    std::chrono::steady_clock::time_point deadline;
    {
      std::unique_lock lock(mutex_);
      available_.wait(lock, [&] {
        return stop_ || state_.load() != ChannelState::Ready || queued_;
      });
      // A caller may open the circuit before this worker is scheduled. Never
      // transmit a queued key after its deadline has already failed open.
      if (stop_ || state_.load() != ChannelState::Ready) {
        if (queued_) {
          queued_ = false;
          {
            std::lock_guard item_lock(work_.mutex);
            work_.result.status = Status::Unavailable;
            work_.done = true;
          }
          work_.completed.notify_all();
        }
        break;
      }
      queued_ = false;
      request = std::move(work_.request);
      deadline = work_.deadline;
      in_flight_ = true;
      pipe = pipe_;
    }

    std::string ignored_error;
    Frame reply;
    pipe_io::Result io = pipe_io::WriteFrame(
        pipe, request, deadline, &ignored_error, retirement_);
    if (io == pipe_io::Result::Ok) {
      io = pipe_io::ReadFrame(pipe, &reply, deadline, &ignored_error,
                              retirement_);
    }

    CallResult result;
    const bool valid = io == pipe_io::Result::Ok &&
                       ValidReply(request, reply) &&
                       reply.correlation.connection_generation ==
                           connection_generation_.load() &&
                       state_.load() == ChannelState::Ready;
    if (!valid) {
      OpenCircuit(PreservesLogicalConnection(request.command));
      result.status = io == pipe_io::Result::Timeout ? Status::Timeout
                                                     : Status::Unavailable;
    } else {
      result.reply = std::move(reply);
      result.status = result.reply.status;
    }
    {
      std::lock_guard lock(mutex_);
      in_flight_ = false;
    }
    {
      std::lock_guard item_lock(work_.mutex);
      work_.result = std::move(result);
      work_.done = true;
    }
    work_.completed.notify_all();
  }
  } catch (...) {
    try {
      OpenCircuit();
    } catch (...) {
    }
    try {
      std::lock_guard lock(mutex_);
      queued_ = false;
      in_flight_ = false;
    } catch (...) {
    }
    try {
      std::lock_guard item_lock(work_.mutex);
      work_.result.status = Status::Unavailable;
      work_.done = true;
    } catch (...) {
    }
    work_.completed.notify_all();
  }
}

void PipeRuntimePort::UiWorkerMain() noexcept {
  try {
  uint64_t observed_epoch = ui_wake_epoch_.load();
  size_t reconnect_failures = 0;
  for (;;) {
    if (ui_stop_.load() || state_.load() != ChannelState::Ready)
      break;
    std::shared_ptr<const Frame> request = posted_request_.exchange(nullptr);
    if (!request) {
      ui_wake_epoch_.wait(observed_epoch);
      observed_epoch = ui_wake_epoch_.load();
      continue;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::shared_ptr<pipe_io::RetirementGate> retirement;
    {
      std::lock_guard lock(ui_mutex_);
      pipe = ui_pipe_;
      retirement = ui_retirement_;
    }
    if (pipe == INVALID_HANDLE_VALUE) {
      auto candidate_retirement = pipe_io::MakeRetirementGate();
      PipeClientIdentity server_identity;
      uint16_t protocol_version = 0;
      std::string ignored_error;
      HANDLE candidate = ConnectPipeChannel(
          ui_endpoint_, ui_expected_server_, ui_connection_identity_,
          std::chrono::steady_clock::now() + kOptionalUiConnectBudget,
          &ignored_error, candidate_retirement, ui_mutex_,
          &ui_connecting_pipe_, &ui_stop_, &server_identity,
          &protocol_version);
      const bool authenticated =
          candidate != INVALID_HANDLE_VALUE &&
          server_identity == ui_expected_identity_ &&
          protocol_version == ui_expected_protocol_ &&
          state_.load() == ChannelState::Ready && !ui_stop_.load();
      {
        std::lock_guard lock(ui_mutex_);
        if (authenticated && !ui_stop_.load() &&
            state_.load() == ChannelState::Ready) {
          ui_pipe_ = candidate;
          ui_retirement_ = std::move(candidate_retirement);
          reconnect_failures = 0;
          pipe = candidate;
          retirement = ui_retirement_;
        } else {
          if (candidate != INVALID_HANDLE_VALUE)
            CloseHandle(candidate);
        }
      }
      if (pipe == INVALID_HANDLE_VALUE) {
        RequeueUiRequest(std::move(request));
        ++reconnect_failures;
        if (ui_stop_.load() || state_.load() != ChannelState::Ready)
          break;
        const size_t shift = std::min<size_t>(reconnect_failures - 1, 7);
        const auto backoff = std::min(
            kUiReconnectInitialBackoff * (1 << shift),
            kUiReconnectMaximumBackoff);
        auto remaining = backoff;
        while (remaining.count() > 0 && !ui_stop_.load() &&
               state_.load() == ChannelState::Ready) {
          const auto slice =
              std::min(remaining, std::chrono::milliseconds(10));
          Sleep(static_cast<DWORD>(slice.count()));
          remaining -= slice;
        }
        continue;
      }
    }

    const auto deadline =
        std::chrono::steady_clock::now() + kHardCallDeadline;

    std::string ignored_error;
    Frame reply;
    pipe_io::Result io = pipe_io::WriteFrame(
        pipe, *request, deadline, &ignored_error, retirement);
    if (io == pipe_io::Result::Ok) {
      io = pipe_io::ReadFrame(pipe, &reply, deadline, &ignored_error,
                              retirement);
    }
    const bool valid = io == pipe_io::Result::Ok &&
                       ValidReply(*request, reply) &&
                       reply.correlation.connection_generation ==
                           connection_generation_.load() &&
                       state_.load() == ChannelState::Ready;
    if (!valid) {
      RequeueUiRequest(std::move(request));
      reconnect_failures = 0;
      {
        std::lock_guard lock(ui_mutex_);
        if (ui_pipe_ == pipe) {
          CloseHandle(ui_pipe_);
          ui_pipe_ = INVALID_HANDLE_VALUE;
        }
      }
      if (ui_stop_.load() || state_.load() != ChannelState::Ready)
        break;
      continue;
    }
    reconnect_failures = 0;
    // UiState is lossy state. Ok applies it; any valid rejection simply drops
    // this obsolete update while leaving both transport lanes usable.
  }
  } catch (...) {
    posted_request_.store(nullptr);
    ui_wake_epoch_.fetch_add(1);
    ui_wake_epoch_.notify_all();
  }
}

void PipeRuntimePort::OpenCircuit(bool preserve_connection_generation) {
  ChannelState expected = ChannelState::Ready;
  if (state_.compare_exchange_strong(expected, ChannelState::OpenCircuit) &&
      !preserve_connection_generation) {
    connection_generation_.fetch_add(1);
  }
  if (expected == ChannelState::Ready ||
      state_.load() == ChannelState::OpenCircuit) {
    server_process_id_.store(0, std::memory_order_release);
    server_creation_time_.store(0, std::memory_order_release);
  }
  available_.notify_all();
  posted_request_.store(nullptr);
  ui_wake_epoch_.fetch_add(1);
  ui_wake_epoch_.notify_all();
}

ChannelState PipeRuntimePort::state() const { return state_.load(); }

uint64_t PipeRuntimePort::connection_generation() const {
  return connection_generation_.load();
}

PipeClientIdentity PipeRuntimePort::server_identity() const noexcept {
  const uint32_t process_id =
      server_process_id_.load(std::memory_order_acquire);
  if (process_id == 0)
    return {};
  return {
      process_id,
      server_creation_time_.load(std::memory_order_acquire),
  };
}

} // namespace famo::runtime
