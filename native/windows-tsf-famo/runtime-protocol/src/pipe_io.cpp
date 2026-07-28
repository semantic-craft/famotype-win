#include "pipe_io.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

#include "win_handle.h"

namespace famo::runtime::pipe_io {

using win::UniqueHandle;

struct PendingTransfer {
  explicit PendingTransfer(size_t size)
      : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), bytes(size) {
    overlapped.hEvent = event.get();
  }

  UniqueHandle event;
  OVERLAPPED overlapped{};
  std::vector<uint8_t> bytes;
};

class RetirementGate {
public:
  bool TryBegin() {
    bool expected = false;
    return active_.compare_exchange_strong(expected, true);
  }
  void End() { active_.store(false); }
  bool Ready() const { return !active_.load(); }
  void Quarantine(std::unique_ptr<PendingTransfer> pending,
                  const RetirementGatePtr &self) {
    std::lock_guard lock(mutex_);
    quarantined_ = std::move(pending);
    self_hold_ = self;
  }

private:
  std::atomic<bool> active_{false};
  std::mutex mutex_;
  std::unique_ptr<PendingTransfer> quarantined_;
  RetirementGatePtr self_hold_;
};

RetirementGatePtr MakeRetirementGate() {
  return std::make_shared<RetirementGate>();
}

bool RetirementReady(const RetirementGatePtr &gate) {
  return !gate || gate->Ready();
}

namespace {

class RetirementLease {
public:
  explicit RetirementLease(RetirementGatePtr gate)
      : gate_(std::move(gate)), owns_(gate_ && gate_->TryBegin()) {}
  ~RetirementLease() {
    if (owns_)
      gate_->End();
  }

  bool acquired() const { return !gate_ || owns_; }
  void PassToReaper() { owns_ = false; }

private:
  RetirementGatePtr gate_;
  bool owns_ = false;
};

struct ReaperContext {
  std::unique_ptr<PendingTransfer> pending;
  RetirementGatePtr gate;
};

DWORD WINAPI ReapPendingTransfer(void *value) noexcept {
  std::unique_ptr<ReaperContext> context(static_cast<ReaperContext *>(value));
  WaitForSingleObject(context->pending->event.get(), INFINITE);
  context->gate->End();
  return 0;
}

bool RetirePendingTransfer(std::unique_ptr<PendingTransfer> pending,
                           const RetirementGatePtr &gate) {
  constexpr DWORD kCancellationGraceMs = 5;
  if (WaitForSingleObject(pending->event.get(), kCancellationGraceMs) ==
      WAIT_OBJECT_0) {
    return false;
  }

  if (!gate) {
    // This path is used only by the isolated test runtime/server process.
    WaitForSingleObject(pending->event.get(), INFINITE);
    return false;
  }

  // A gate is acquired before issuing I/O, so one port can retain at most one
  // cancellation. Until it completes, reconnect fails closed without issuing
  // another transfer.
  auto context = std::make_unique<ReaperContext>();
  context->pending = std::move(pending);
  context->gate = gate;
  HANDLE reaper =
      CreateThread(nullptr, 0, ReapPendingTransfer, context.get(), 0, nullptr);
  if (reaper) {
    [[maybe_unused]] ReaperContext *transferred_context = context.release();
    CloseHandle(reaper);
  } else {
    // Resource exhaustion: retain one owned transfer and permanently fail this
    // gate closed. Never free memory that may still be owned by the kernel.
    gate->Quarantine(std::move(context->pending), gate);
  }
  return true;
}

bool IsDisconnect(DWORD error) {
  return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
         error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED;
}

DWORD Remaining(Deadline deadline) {
  if (deadline == Deadline::max())
    return INFINITE;
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<DWORD>(std::clamp<int64_t>(
      remaining.count() + 1, 1, std::numeric_limits<DWORD>::max() - 1));
}

Result Transfer(HANDLE pipe, uint8_t *data, size_t size, bool write,
                Deadline deadline, std::string *error,
                const RetirementGatePtr &gate) {
  size_t offset = 0;
  while (offset < size) {
    if (Remaining(deadline) == 0)
      return Result::Timeout;
    RetirementLease lease(gate);
    if (!lease.acquired())
      return Result::Failed;

    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(size - offset, std::numeric_limits<DWORD>::max()));
    auto pending = std::make_unique<PendingTransfer>(chunk);
    if (!pending->event) {
      if (error)
        *error = "CreateEvent failed: " + std::to_string(GetLastError());
      return Result::Failed;
    }
    if (write)
      std::memcpy(pending->bytes.data(), data + offset, chunk);
    DWORD transferred = 0;
    const BOOL immediate = write ? WriteFile(pipe, pending->bytes.data(), chunk,
                                             &transferred, &pending->overlapped)
                                 : ReadFile(pipe, pending->bytes.data(), chunk,
                                            &transferred, &pending->overlapped);
    if (!immediate) {
      const DWORD start_error = GetLastError();
      if (start_error != ERROR_IO_PENDING) {
        if (IsDisconnect(start_error))
          return Result::Disconnected;
        if (error)
          *error = "pipe I/O start failed: " + std::to_string(start_error);
        return Result::Failed;
      }
      const DWORD wait =
          WaitForSingleObject(pending->event.get(), Remaining(deadline));
      if (wait == WAIT_TIMEOUT) {
        CancelIoEx(pipe, &pending->overlapped);
        if (RetirePendingTransfer(std::move(pending), gate))
          lease.PassToReaper();
        if (error)
          *error = "pipe I/O deadline elapsed";
        return Result::Timeout;
      }
      if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &pending->overlapped);
        if (RetirePendingTransfer(std::move(pending), gate))
          lease.PassToReaper();
        if (error)
          *error = "pipe I/O wait failed: " + std::to_string(GetLastError());
        return Result::Failed;
      }
      if (!GetOverlappedResult(pipe, &pending->overlapped, &transferred,
                               FALSE)) {
        const DWORD finish_error = GetLastError();
        if (IsDisconnect(finish_error))
          return Result::Disconnected;
        if (error) {
          *error =
              "pipe I/O completion failed: " + std::to_string(finish_error);
        }
        return Result::Failed;
      }
    }
    if (transferred == 0)
      return Result::Disconnected;
    if (!write)
      std::memcpy(data + offset, pending->bytes.data(), transferred);
    offset += transferred;
  }
  return Result::Ok;
}

} // namespace

Result WriteBytes(HANDLE pipe, std::span<const uint8_t> bytes,
                  Deadline deadline, std::string *error,
                  const RetirementGatePtr &gate) {
  return Transfer(pipe, const_cast<uint8_t *>(bytes.data()), bytes.size(), true,
                  deadline, error, gate);
}

Result ReadBytes(HANDLE pipe, std::span<uint8_t> bytes, Deadline deadline,
                 std::string *error, const RetirementGatePtr &gate) {
  return Transfer(pipe, bytes.data(), bytes.size(), false, deadline, error,
                  gate);
}

Result WriteFrame(HANDLE pipe, const Frame &frame, Deadline deadline,
                  std::string *error, const RetirementGatePtr &gate) {
  std::vector<uint8_t> bytes;
  if (!EncodeFrame(frame, &bytes, error))
    return Result::Failed;
  return WriteBytes(pipe, bytes, deadline, error, gate);
}

Result ReadFrame(HANDLE pipe, Frame *frame, Deadline deadline,
                 std::string *error, const RetirementGatePtr &gate) {
  std::vector<uint8_t> bytes(kHeaderSize);
  Result result = ReadBytes(pipe, bytes, deadline, error, gate);
  if (result != Result::Ok)
    return result;
  uint32_t size = 0;
  if (!PeekFrameSize(bytes, &size, error))
    return Result::Failed;
  bytes.resize(size);
  if (size > kHeaderSize) {
    result = ReadBytes(pipe, std::span<uint8_t>(bytes).subspan(kHeaderSize),
                       deadline, error, gate);
    if (result != Result::Ok)
      return result;
  }
  return DecodeFrame(bytes, frame, error) ? Result::Ok : Result::Failed;
}

} // namespace famo::runtime::pipe_io
