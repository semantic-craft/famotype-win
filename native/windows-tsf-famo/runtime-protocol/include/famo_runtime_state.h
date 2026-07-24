#pragma once

#include <chrono>
#include <functional>

#include "famo_runtime_protocol.h"

namespace famo::runtime {

enum class ChannelState { NotReady, Ready, OpenCircuit };
enum class FaultKind {
  None,
  ConnectHang,
  WriteHang,
  ReadHang,
  EngineHang,
  UiHang,
  Disconnect,
  MalformedReply,
  LateReply
};

struct CallResult {
  Status status = Status::Unavailable;
  Frame reply;
  std::chrono::milliseconds elapsed{0};
};

class CorrelationState {
public:
  void Reset(const Correlation &generation);
  bool AcceptRequest(const Correlation &correlation);
  bool AcceptReply(const Correlation &request, const Correlation &reply) const;
  void Poison();
  uint64_t connection_generation() const { return connection_generation_; }

private:
  uint64_t client_id_ = 0;
  uint64_t activation_generation_ = 0;
  uint64_t connection_generation_ = 0;
  uint64_t session_id_ = 0;
  uint64_t session_generation_ = 0;
  uint64_t last_sequence_ = 0;
};

class InMemoryRuntimePort {
public:
  using Handler = std::function<Frame(const Frame &)>;

  explicit InMemoryRuntimePort(Handler handler);
  void SetReady(uint64_t connection_generation);
  void Inject(FaultKind fault, std::chrono::milliseconds delay);
  CallResult Call(const Frame &request, std::chrono::milliseconds deadline);
  ChannelState state() const { return state_; }
  uint64_t connection_generation() const { return connection_generation_; }

private:
  void OpenCircuit();

  Handler handler_;
  ChannelState state_ = ChannelState::NotReady;
  uint64_t connection_generation_ = 0;
  FaultKind fault_ = FaultKind::None;
  std::chrono::milliseconds delay_{0};
};

} // namespace famo::runtime
