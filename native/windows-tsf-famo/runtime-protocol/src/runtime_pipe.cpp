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

DWORD Remaining(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  const auto value =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<DWORD>(std::max<int64_t>(1, value.count()));
}

bool ValidReply(const Frame &request, const Frame &reply) {
  return reply.flags == kFlagResponse && reply.command == request.command &&
         reply.correlation == request.correlation;
}

bool SameSession(const Correlation &left, const Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation &&
         left.session_id == right.session_id &&
         left.session_generation == right.session_generation;
}

} // namespace

PipeRuntimePort::PipeRuntimePort()
    : retirement_(pipe_io::MakeRetirementGate()),
      ui_retirement_(pipe_io::MakeRetirementGate()) {}

PipeRuntimePort::~PipeRuntimePort() { Stop(); }

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
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto connect_pipe = [&](const PipeEndpoint &channel_endpoint,
                                std::string *channel_error,
                                const auto &retirement,
                                auto connect_deadline) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (pipe == INVALID_HANDLE_VALUE &&
           std::chrono::steady_clock::now() < connect_deadline &&
           (!cancelled || !cancelled->load())) {
      HANDLE candidate =
          CreateFileW(channel_endpoint.name.c_str(),
                      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OVERLAPPED, nullptr);
      if (candidate != INVALID_HANDLE_VALUE) {
        {
          std::lock_guard lock(connect_mutex_);
          connecting_pipe_ = candidate;
        }
        const auto release_candidate = [&](bool close) {
          {
            std::lock_guard lock(connect_mutex_);
            if (connecting_pipe_ == candidate)
              connecting_pipe_ = INVALID_HANDLE_VALUE;
          }
          if (close)
            CloseHandle(candidate);
        };
        if (cancelled && cancelled->load()) {
          release_candidate(true);
          break;
        }
        if (!VerifyPipeServer(candidate, channel_endpoint, expected_server,
                              channel_error)) {
          release_candidate(true);
          return INVALID_HANDLE_VALUE;
        }
        DWORD mode = PIPE_READMODE_BYTE;
        if (!SetNamedPipeHandleState(candidate, &mode, nullptr, nullptr)) {
          if (channel_error) {
            *channel_error = "SetNamedPipeHandleState failed: " +
                             std::to_string(GetLastError());
          }
          release_candidate(true);
          return INVALID_HANDLE_VALUE;
        }

        Frame hello;
        hello.command = Command::Hello;
        hello.correlation = connection_identity;
        Frame hello_reply;
        const pipe_io::Result hello_write = pipe_io::WriteFrame(
            candidate, hello, connect_deadline, channel_error, retirement);
        const pipe_io::Result hello_read =
            hello_write == pipe_io::Result::Ok
                ? pipe_io::ReadFrame(candidate, &hello_reply, connect_deadline,
                                     channel_error, retirement)
                : hello_write;
        if (hello_read == pipe_io::Result::Ok &&
            ValidReply(hello, hello_reply) &&
            hello_reply.status == Status::Ok &&
            (!cancelled || !cancelled->load())) {
          pipe = candidate;
          release_candidate(false);
          break;
        }
        const bool retryable = hello_read == pipe_io::Result::Ok &&
                               ValidReply(hello, hello_reply) &&
                               hello_reply.status == Status::Unavailable;
        release_candidate(true);
        if (cancelled && cancelled->load())
          break;
        if (!retryable) {
          if (channel_error && channel_error->empty())
            *channel_error = "pipe Hello handshake failed";
          return INVALID_HANDLE_VALUE;
        }
        if (channel_error)
          channel_error->clear();
        Sleep(std::min<DWORD>(2, Remaining(connect_deadline)));
        continue;
      }
      const DWORD open_error = GetLastError();
      if (open_error == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(channel_endpoint.name.c_str(),
                       std::min<DWORD>(10, Remaining(connect_deadline)));
      } else if (open_error == ERROR_FILE_NOT_FOUND) {
        Sleep(std::min<DWORD>(2, Remaining(connect_deadline)));
      } else {
        if (channel_error) {
          *channel_error =
              "CreateFile(pipe) failed: " + std::to_string(open_error);
        }
        return INVALID_HANDLE_VALUE;
      }
    }
    if (pipe == INVALID_HANDLE_VALUE && channel_error)
      *channel_error = "pipe connect deadline elapsed";
    return pipe;
  };

  HANDLE pipe = connect_pipe(endpoint, error, retirement_, deadline);
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  // A cancelled best-effort transfer may still be retiring on its old gate.
  // The reaper retains that gate, so a reconnect can start a fresh optional UI
  // lane without weakening the primary lane's fail-closed retirement rule.
  ui_retirement_ = pipe_io::MakeRetirementGate();
  std::string ignored_ui_error;
  const auto ui_deadline =
      std::min(deadline, std::chrono::steady_clock::now() +
                             kOptionalUiConnectBudget);
  const PipeEndpoint ui_endpoint = BuildUiPipeEndpoint(endpoint);
  HANDLE ui_pipe = connect_pipe(ui_endpoint, &ignored_ui_error,
                                ui_retirement_, ui_deadline);
  {
    std::lock_guard lock(mutex_);
    pipe_ = pipe;
    connection_generation_.store(connection_identity.connection_generation);
    state_.store(ChannelState::Ready);
    slot_busy_.store(false);
    stop_ = false;
    queued_ = false;
    in_flight_ = false;
  }
  {
    std::lock_guard lock(ui_mutex_);
    ui_pipe_ = ui_pipe;
    ui_stop_ = false;
    ui_ready_ = ui_pipe != INVALID_HANDLE_VALUE;
    posted_request_.store(nullptr);
  }
  try {
    worker_ = std::thread(&PipeRuntimePort::WorkerMain, this);
  } catch (...) {
    std::lock_guard lock(mutex_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    state_.store(ChannelState::NotReady);
    slot_busy_.store(false);
    if (ui_pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(ui_pipe);
      std::lock_guard ui_lock(ui_mutex_);
      ui_pipe_ = INVALID_HANDLE_VALUE;
      ui_ready_ = false;
    }
    if (error)
      *error = "pipe worker creation failed";
    return false;
  }
  if (ui_pipe != INVALID_HANDLE_VALUE) {
    try {
      ui_worker_ = std::thread(&PipeRuntimePort::UiWorkerMain, this);
    } catch (...) {
      std::lock_guard lock(ui_mutex_);
      CloseHandle(ui_pipe_);
      ui_pipe_ = INVALID_HANDLE_VALUE;
      ui_ready_ = false;
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

void PipeRuntimePort::CancelCall() {
  OpenCircuit();
  std::lock_guard lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE)
    CancelIoEx(pipe_, nullptr);
}

void PipeRuntimePort::Stop() {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE ui_pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    pipe = pipe_;
  }
  {
    std::lock_guard lock(ui_mutex_);
    ui_stop_ = true;
    ui_pipe = ui_pipe_;
  }
  available_.notify_all();
  ui_wake_epoch_.fetch_add(1);
  ui_wake_epoch_.notify_all();
  if (pipe != INVALID_HANDLE_VALUE)
    CancelIoEx(pipe, nullptr);
  if (ui_pipe != INVALID_HANDLE_VALUE)
    CancelIoEx(ui_pipe, nullptr);
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
    ui_stop_ = false;
    ui_ready_ = false;
  }
}

CallResult PipeRuntimePort::Call(Frame &&request,
                                 std::chrono::milliseconds deadline) {
  const auto started = std::chrono::steady_clock::now();
  CallResult immediate;
  const std::chrono::milliseconds maximum =
      request.command == Command::OpenSession ? kSessionOpenDeadline
                                               : kHardCallDeadline;
  deadline = std::min(deadline, maximum);
  if (deadline.count() <= 0)
    return immediate;

  const auto wait_budget =
      std::max(std::chrono::milliseconds(1), deadline - kWaitSchedulingReserve);
  const auto absolute_deadline = started + wait_budget;
  if (state_.load() != ChannelState::Ready || slot_busy_.load()) {
    immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return immediate;
  }
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_until(absolute_deadline)) {
    if (state_.load() == ChannelState::Ready) {
      OpenCircuit();
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
  work_.request = std::move(request);
  work_.deadline = absolute_deadline;
  work_.result = {};
  work_.done = false;
  slot_busy_.store(true);
  queued_ = true;
  lock.unlock();
  available_.notify_one();

  std::unique_lock item_lock(work_.mutex);
  const bool done = work_.completed.wait_until(item_lock, absolute_deadline,
                                               [&] { return work_.done; });
  const auto completed_at = std::chrono::steady_clock::now();
  if (done && completed_at < absolute_deadline) {
    work_.result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(completed_at -
                                                              started);
    CallResult result = std::move(work_.result);
    item_lock.unlock();
    slot_busy_.store(false);
    return result;
  }
  item_lock.unlock();
  OpenCircuit();
  if (done)
    slot_busy_.store(false);
  immediate.status = Status::Timeout;
  immediate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      completed_at - started);
  return immediate;
}

void PipeRuntimePort::Post(Frame &&request) {
  if (state_.load() != ChannelState::Ready ||
      request.command != Command::UpdateUiState ||
      request.correlation.connection_generation !=
          connection_generation_.load() || ui_stop_.load() ||
      !ui_ready_.load()) {
    return;
  }
  try {
    auto next = std::make_shared<const Frame>(std::move(request));
    std::shared_ptr<const Frame> pending = posted_request_.load();
    for (;;) {
      if (pending && SameSession(pending->correlation, next->correlation) &&
          next->correlation.sequence <= pending->correlation.sequence) {
        return;
      }
      if (posted_request_.compare_exchange_weak(pending, next)) {
        ui_wake_epoch_.fetch_add(1);
        ui_wake_epoch_.notify_one();
        return;
      }
    }
  } catch (...) {
    // UiState is best-effort; allocation failure must not escape into TSF.
  }
}

void PipeRuntimePort::Poison() { OpenCircuit(); }

void PipeRuntimePort::WorkerMain() {
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
      request = work_.request;
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
      OpenCircuit();
      result.status = io == pipe_io::Result::Timeout ? Status::Timeout
                                                     : Status::Unavailable;
    } else if (reply.status == Status::Ok) {
      result.reply = std::move(reply);
      result.status = Status::Ok;
    } else if (reply.status == Status::Unavailable) {
      result.reply = std::move(reply);
      result.status = Status::Unavailable;
    } else {
      OpenCircuit();
      result.status = Status::Unavailable;
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
}

void PipeRuntimePort::UiWorkerMain() {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(ui_mutex_);
    pipe = ui_pipe_;
  }
  uint64_t observed_epoch = ui_wake_epoch_.load();
  for (;;) {
    if (ui_stop_.load() || !ui_ready_.load())
      break;
    std::shared_ptr<const Frame> request = posted_request_.exchange(nullptr);
    if (!request) {
      ui_wake_epoch_.wait(observed_epoch);
      observed_epoch = ui_wake_epoch_.load();
      continue;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + kHardCallDeadline;

    std::string ignored_error;
    Frame reply;
    pipe_io::Result io = pipe_io::WriteFrame(
        pipe, *request, deadline, &ignored_error, ui_retirement_);
    if (io == pipe_io::Result::Ok) {
      io = pipe_io::ReadFrame(pipe, &reply, deadline, &ignored_error,
                              ui_retirement_);
    }
    const bool valid = io == pipe_io::Result::Ok &&
                       ValidReply(*request, reply) &&
                       reply.correlation.connection_generation ==
                           connection_generation_.load() &&
                       state_.load() == ChannelState::Ready;
    if (!valid) {
      ui_ready_ = false;
      posted_request_.store(nullptr);
      ui_wake_epoch_.fetch_add(1);
      ui_wake_epoch_.notify_all();
      break;
    }
    // UiState is lossy state. Ok applies it; any valid rejection simply drops
    // this obsolete update while leaving both transport lanes usable.
  }
}

void PipeRuntimePort::OpenCircuit() {
  ChannelState expected = ChannelState::Ready;
  if (state_.compare_exchange_strong(expected, ChannelState::OpenCircuit)) {
    connection_generation_.fetch_add(1);
  }
  available_.notify_all();
  ui_ready_ = false;
  posted_request_.store(nullptr);
  ui_wake_epoch_.fetch_add(1);
  ui_wake_epoch_.notify_all();
}

ChannelState PipeRuntimePort::state() const { return state_.load(); }

uint64_t PipeRuntimePort::connection_generation() const {
  return connection_generation_.load();
}

} // namespace famo::runtime
