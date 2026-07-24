#include "famo_runtime_state.h"

#include <thread>

namespace famo::runtime {

void CorrelationState::Reset(const Correlation &generation) {
  client_id_ = generation.client_id;
  activation_generation_ = generation.activation_generation;
  connection_generation_ = generation.connection_generation;
  session_id_ = 0;
  session_generation_ = 0;
  last_sequence_ = 0;
}

bool CorrelationState::AcceptRequest(const Correlation &value) {
  if (value.client_id != client_id_ ||
      value.activation_generation != activation_generation_ ||
      value.connection_generation != connection_generation_ ||
      value.session_id == 0 || value.session_generation == 0 ||
      value.sequence == 0 || value.sequence <= last_sequence_)
    return false;
  if (session_id_ == 0) {
    session_id_ = value.session_id;
    session_generation_ = value.session_generation;
  }
  if (value.session_id != session_id_ ||
      value.session_generation != session_generation_)
    return false;
  last_sequence_ = value.sequence;
  return true;
}

bool CorrelationState::AcceptReply(const Correlation &request,
                                   const Correlation &reply) const {
  return request == reply && reply.client_id == client_id_ &&
         reply.activation_generation == activation_generation_ &&
         reply.connection_generation == connection_generation_ &&
         reply.session_id == session_id_ &&
         reply.session_generation == session_generation_ &&
         reply.sequence == last_sequence_;
}

void CorrelationState::Poison() {
  ++connection_generation_;
  session_id_ = 0;
  session_generation_ = 0;
  last_sequence_ = 0;
}

InMemoryRuntimePort::InMemoryRuntimePort(Handler handler)
    : handler_(std::move(handler)) {}

void InMemoryRuntimePort::SetReady(uint64_t connection_generation) {
  connection_generation_ = connection_generation;
  state_ = ChannelState::Ready;
  fault_ = FaultKind::None;
  delay_ = std::chrono::milliseconds(0);
}

void InMemoryRuntimePort::Inject(FaultKind fault,
                                 std::chrono::milliseconds delay) {
  fault_ = fault;
  delay_ = delay;
}

void InMemoryRuntimePort::OpenCircuit() {
  state_ = ChannelState::OpenCircuit;
  ++connection_generation_;
}

CallResult InMemoryRuntimePort::Call(const Frame &request,
                                     std::chrono::milliseconds deadline) {
  const auto start = std::chrono::steady_clock::now();
  CallResult result;
  if (state_ != ChannelState::Ready ||
      request.correlation.connection_generation != connection_generation_) {
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
  }

  const bool hangs =
      fault_ == FaultKind::ConnectHang || fault_ == FaultKind::WriteHang ||
      fault_ == FaultKind::ReadHang || fault_ == FaultKind::EngineHang ||
      fault_ == FaultKind::UiHang;
  if (hangs && delay_ >= deadline) {
    std::this_thread::sleep_for(deadline);
    OpenCircuit();
    result.status = Status::Timeout;
  } else if (fault_ == FaultKind::Disconnect ||
             fault_ == FaultKind::MalformedReply) {
    OpenCircuit();
    result.status = Status::Unavailable;
  } else {
    if (delay_.count() > 0)
      std::this_thread::sleep_for(delay_);
    result.reply = handler_(request);
    if (fault_ == FaultKind::LateReply &&
        result.reply.correlation.connection_generation > 0)
      --result.reply.correlation.connection_generation;
    if (result.reply.correlation != request.correlation ||
        result.reply.status != Status::Ok) {
      OpenCircuit();
      result.status = Status::Unavailable;
    } else {
      result.status = Status::Ok;
    }
  }
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  return result;
}

} // namespace famo::runtime
