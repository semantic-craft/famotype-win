#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "famo_runtime_protocol.h"

namespace famo::tsf {

enum class ContextPhase { Unopened, Ready, Poisoned, Closed };
enum class RequestKind { None, ProcessKey };

struct HostKey {
  uint32_t virtual_key = 0;
  uint32_t scan_code = 0;
  uint32_t modifiers = 0;
  bool is_key_down = false;
  uint64_t timestamp_ms = 0;
};

struct KeyPlan {
  RequestKind request = RequestKind::None;
  runtime::Correlation correlation;
  runtime::KeyEvent key;

  bool sends_request() const { return request != RequestKind::None; }
};

struct RecoveryPlan {
  std::optional<std::string> commit_preedit;
};

class ContextState {
public:
  void Open(const runtime::Correlation &session_identity);
  void Close();

  bool TestKey(const HostKey &key) const;
  KeyPlan PlanKey(const HostKey &key);
  std::optional<runtime::Correlation>
  PlanAbsoluteCandidate(uint64_t composition_sequence);
  std::optional<runtime::Correlation> PlanUiState();
  std::optional<runtime::Correlation> PlanClose();
  bool AcceptReply(const runtime::Correlation &reply) const;
  void CompleteUnhandled();
  void ApplySucceeded(const runtime::Composition &composition);
  RecoveryPlan Fail();

  ContextPhase phase() const { return phase_; }
  uint64_t pending_sequence() const { return pending_sequence_; }
  uint64_t displayed_sequence() const { return displayed_sequence_; }
  const runtime::Correlation &session_identity() const { return identity_; }
  const runtime::Composition &displayed() const { return displayed_; }

private:
  bool Classifies(const HostKey &key) const;

  ContextPhase phase_ = ContextPhase::Unopened;
  runtime::Correlation identity_;
  runtime::Composition displayed_;
  uint64_t next_sequence_ = 1;
  uint64_t pending_sequence_ = 0;
  uint64_t displayed_sequence_ = 0;
  bool recovery_emitted_ = false;
};

} // namespace famo::tsf
