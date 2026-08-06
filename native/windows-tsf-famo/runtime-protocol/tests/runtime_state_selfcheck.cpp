#include <chrono>
#include <cstdio>

#include "famo_runtime_state.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;
using namespace std::chrono_literals;

Frame Echo(const Frame &request) {
  Frame reply = request;
  reply.flags = kFlagResponse;
  return reply;
}

int main() {
  CorrelationState correlation;
  correlation.Reset({.client_id = 10,
                     .activation_generation = 20,
                     .connection_generation = 30});
  Correlation first{10, 20, 30, 40, 50, 1};
  Correlation invalid = first;
  invalid.session_id = 0;
  CHECK(!correlation.AcceptRequest(invalid));
  invalid = first;
  invalid.session_generation = 0;
  CHECK(!correlation.AcceptRequest(invalid));
  invalid = first;
  ++invalid.activation_generation;
  CHECK(!correlation.AcceptRequest(invalid));
  CHECK(correlation.AcceptRequest(first));
  CHECK(!correlation.AcceptRequest(first));
  Correlation backward = first;
  backward.sequence = 0;
  CHECK(!correlation.AcceptRequest(backward));
  Correlation next = first;
  next.sequence = 2;
  Correlation wrong_session = next;
  ++wrong_session.session_generation;
  CHECK(!correlation.AcceptRequest(wrong_session));
  CHECK(correlation.AcceptRequest(next));
  CHECK(correlation.AcceptReply(next, next));
  Correlation stale = next;
  --stale.connection_generation;
  CHECK(!correlation.AcceptReply(next, stale));
  correlation.Poison();
  CHECK(correlation.connection_generation() == 31);
  CHECK(!correlation.AcceptRequest(next));

  for (FaultKind fault :
       {FaultKind::ConnectHang, FaultKind::WriteHang, FaultKind::ReadHang,
        FaultKind::EngineHang, FaultKind::UiHang}) {
    InMemoryRuntimePort port(Echo);
    port.SetReady(7);
    port.Inject(fault, 2s);
    Frame request;
    request.command = Command::ProcessKey;
    request.correlation = {1, 1, 7, 1, 1, 1};
    const CallResult first_call = port.Call(request, 50ms);
    CHECK(first_call.status == Status::Timeout);
    // sleep_for may overshoot on a loaded Windows runner. Keep a generous
    // scheduler allowance while still proving the call returns far before the
    // injected two-second stall.
    CHECK(first_call.elapsed >= 45ms && first_call.elapsed <= 250ms);
    const CallResult second_call = port.Call(request, 50ms);
    CHECK(second_call.status == Status::Unavailable);
    CHECK(second_call.elapsed < 5ms);
  }

  InMemoryRuntimePort late(Echo);
  late.SetReady(9);
  late.Inject(FaultKind::LateReply, 0ms);
  Frame request;
  request.command = Command::ProcessKey;
  request.correlation = {1, 1, 9, 1, 1, 1};
  CHECK(late.Call(request, 50ms).status == Status::Unavailable);
  CHECK(late.state() == ChannelState::OpenCircuit);

  InMemoryRuntimePort malformed(Echo);
  malformed.SetReady(11);
  malformed.Inject(FaultKind::MalformedReply, 0ms);
  CHECK(malformed
            .Call(Frame{Command::Hello, 0, Status::Ok, {1, 1, 11, 1, 1, 1}, {}},
                  50ms)
            .status == Status::Unavailable);
  CHECK(malformed.Call(request, 50ms).elapsed < 5ms);

  InMemoryRuntimePort disconnected(Echo);
  disconnected.SetReady(12);
  disconnected.Inject(FaultKind::Disconnect, 0ms);
  request.correlation.connection_generation = 12;
  CHECK(disconnected.Call(request, 50ms).status == Status::Unavailable);
  CHECK(disconnected.state() == ChannelState::OpenCircuit);
  CHECK(disconnected.Call(request, 50ms).elapsed < 5ms);

  std::printf("runtime_state_selfcheck: OK\n");
  return 0;
}
