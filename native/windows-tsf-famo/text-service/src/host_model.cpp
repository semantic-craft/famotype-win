#include "famo_tsf_host_model.h"

#include <algorithm>
#include <utility>

namespace famo::tsf {

namespace {

constexpr uint32_t kRimeBackSpace = 0xff08;
constexpr uint32_t kRimeTab = 0xff09;
constexpr uint32_t kRimeReturn = 0xff0d;
constexpr uint32_t kRimeEscape = 0xff1b;
constexpr uint32_t kRimeHome = 0xff50;
constexpr uint32_t kRimeLeft = 0xff51;
constexpr uint32_t kRimeUp = 0xff52;
constexpr uint32_t kRimeRight = 0xff53;
constexpr uint32_t kRimeDown = 0xff54;
constexpr uint32_t kRimePageUp = 0xff55;
constexpr uint32_t kRimePageDown = 0xff56;
constexpr uint32_t kRimeEnd = 0xff57;
constexpr uint32_t kRimeInsert = 0xff63;
constexpr uint32_t kRimeDelete = 0xffff;
constexpr uint32_t kRimeF4 = 0xffc1;
constexpr uint32_t kRimeShiftLeft = 0xffe1;
constexpr uint32_t kRimeShiftRight = 0xffe2;
constexpr uint32_t kRimeCapsLock = 0xffe5;

bool IsLetter(uint32_t key) {
  return (key >= 'A' && key <= 'Z') || (key >= 'a' && key <= 'z');
}

bool IsModifier(uint32_t key) {
  return key == kRimeShiftLeft || key == kRimeShiftRight ||
         key == kRimeCapsLock;
}

bool IsCompositionKey(uint32_t key) {
  switch (key) {
  case kRimeBackSpace:
  case kRimeTab:
  case kRimeReturn:
  case kRimeEscape:
  case kRimeHome:
  case kRimeLeft:
  case kRimeUp:
  case kRimeRight:
  case kRimeDown:
  case kRimePageUp:
  case kRimePageDown:
  case kRimeEnd:
  case kRimeInsert:
  case kRimeDelete:
    return true;
  default:
    return false;
  }
}

} // namespace

std::optional<runtime::UiRect>
NormalizeLayoutCaret(runtime::UiRect caret,
                     const runtime::UiRect &view_bounds, bool clipped) {
  const int64_t edge_tolerance = clipped ? 0 : 2;
  if (view_bounds.left >= view_bounds.right ||
      view_bounds.top >= view_bounds.bottom || caret.left > caret.right ||
      caret.top >= caret.bottom) {
    return std::nullopt;
  }

  const int64_t left = caret.left;
  const int64_t top = caret.top;
  const int64_t right = caret.right;
  const int64_t bottom = caret.bottom;
  if (right < static_cast<int64_t>(view_bounds.left) - edge_tolerance ||
      left > static_cast<int64_t>(view_bounds.right) + edge_tolerance ||
      bottom < static_cast<int64_t>(view_bounds.top) - edge_tolerance ||
      top > static_cast<int64_t>(view_bounds.bottom) + edge_tolerance) {
    return std::nullopt;
  }

  caret.left = (std::clamp)(caret.left, view_bounds.left, view_bounds.right);
  caret.right =
      (std::clamp)(caret.right, view_bounds.left, view_bounds.right);
  caret.top = (std::clamp)(caret.top, view_bounds.top, view_bounds.bottom);
  caret.bottom =
      (std::clamp)(caret.bottom, view_bounds.top, view_bounds.bottom);
  if (caret.left > caret.right || caret.top >= caret.bottom)
    return std::nullopt;
  return caret;
}

void ContextState::Open(const runtime::Correlation &session_identity) {
  identity_ = session_identity;
  next_sequence_ = identity_.sequence + 1;
  identity_.sequence = 0;
  displayed_ = {};
  pending_sequence_ = 0;
  displayed_sequence_ = 0;
  recovery_emitted_ = false;
  phase_ = ContextPhase::Ready;
}

void ContextState::Close() {
  phase_ = ContextPhase::Closed;
  pending_sequence_ = 0;
  displayed_ = {};
}

bool ContextState::Classifies(const HostKey &key) const {
  if (phase_ != ContextPhase::Ready || key.virtual_key == 0)
    return false;
  if (!key.is_key_down)
    return IsModifier(key.virtual_key);
  if (IsModifier(key.virtual_key) || key.virtual_key == kRimeF4)
    return true;
  if (IsLetter(key.virtual_key))
    return true;
  // Printable keys, including digits, are schema input even with an empty
  // preedit. Digit-start schemes must get the physical key before there is a
  // composition; the engine remains the sole authority on whether it handles
  // that key or uses it as a candidate selector.
  if (key.virtual_key >= 0x21 && key.virtual_key <= 0x7e)
    return true;
  const bool composing = !displayed_.preedit.empty();
  if (!composing)
    return false;
  return IsCompositionKey(key.virtual_key) || key.virtual_key == ' ';
}

bool ContextState::TestKey(const HostKey &key) const {
  return Classifies(key);
}

KeyPlan ContextState::PlanKey(const HostKey &key) {
  KeyPlan plan;
  if (pending_sequence_ != 0 || !Classifies(key))
    return plan;

  plan.correlation = identity_;
  plan.correlation.sequence = next_sequence_++;
  pending_sequence_ = plan.correlation.sequence;

  plan.request = RequestKind::ProcessKey;
  plan.key = {key.virtual_key, key.scan_code, key.modifiers,
              key.is_key_down ? 1u : 0u, key.timestamp_ms};
  return plan;
}

std::optional<runtime::Correlation>
ContextState::PlanAbsoluteCandidate(uint64_t composition_sequence) {
  if (phase_ != ContextPhase::Ready || pending_sequence_ != 0 ||
      composition_sequence == 0 ||
      composition_sequence != displayed_sequence_)
    return std::nullopt;
  runtime::Correlation correlation = identity_;
  correlation.sequence = next_sequence_++;
  pending_sequence_ = correlation.sequence;
  return correlation;
}

std::optional<runtime::Correlation> ContextState::PlanUiState() {
  if (phase_ != ContextPhase::Ready || pending_sequence_ != 0)
    return std::nullopt;
  runtime::Correlation correlation = identity_;
  correlation.sequence = next_sequence_++;
  return correlation;
}

std::optional<runtime::Correlation> ContextState::PlanSecurityUiState() {
  if (phase_ != ContextPhase::Ready)
    return std::nullopt;
  runtime::Correlation correlation = identity_;
  correlation.sequence = next_sequence_++;
  return correlation;
}

std::optional<runtime::Correlation> ContextState::PlanClose() {
  if (phase_ != ContextPhase::Ready || pending_sequence_ != 0)
    return std::nullopt;
  runtime::Correlation correlation = identity_;
  correlation.sequence = next_sequence_++;
  pending_sequence_ = correlation.sequence;
  return correlation;
}

bool ContextState::AcceptReply(const runtime::Correlation &reply) const {
  if (phase_ != ContextPhase::Ready || pending_sequence_ == 0)
    return false;
  runtime::Correlation expected = identity_;
  expected.sequence = pending_sequence_;
  return reply == expected;
}

void ContextState::CompleteUnhandled() {
  if (phase_ == ContextPhase::Ready)
    pending_sequence_ = 0;
}

void ContextState::ApplySucceeded(
    const runtime::Composition &composition) {
  if (phase_ != ContextPhase::Ready || pending_sequence_ == 0)
    return;
  displayed_ = composition;
  displayed_sequence_ = pending_sequence_;
  pending_sequence_ = 0;
}

RecoveryPlan ContextState::Fail() {
  RecoveryPlan plan;
  if (phase_ == ContextPhase::Poisoned || phase_ == ContextPhase::Closed)
    return plan;

  phase_ = ContextPhase::Poisoned;
  pending_sequence_ = 0;
  if (!recovery_emitted_ && !displayed_.preedit.empty()) {
    plan.commit_preedit = displayed_.preedit;
    recovery_emitted_ = true;
  }
  displayed_ = {};
  return plan;
}

} // namespace famo::tsf
