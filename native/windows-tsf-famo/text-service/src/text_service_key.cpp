#include "text_service.h"

#include <new>
#include <utility>

#include "abi_boundary.h"

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
constexpr uint32_t kRimeShiftLeft = 0xffe1;
constexpr uint32_t kRimeShiftRight = 0xffe2;
constexpr uint32_t kRimeControlLeft = 0xffe3;
constexpr uint32_t kRimeControlRight = 0xffe4;
constexpr uint32_t kRimeCapsLock = 0xffe5;
constexpr uint32_t kRimeAltLeft = 0xffe9;
constexpr uint32_t kRimeAltRight = 0xffea;
constexpr uint32_t kRimeKeypadEnter = 0xff8d;
constexpr uint32_t kRimeF1 = 0xffbe;
// Weasel compresses high IBus modifier bits before IPC and expands them again
// in RimeWithWeasel.  This TSF talks to librime through the engine ABI directly,
// so a release must use librime's expanded bit rather than Weasel's wire bit 14.
constexpr uint32_t kRimeReleaseMask = 1u << 30;
constexpr UINT kToUnicodeNoStateChange = 1u << 2;

class ScopedKernelHandle {
public:
  explicit ScopedKernelHandle(HANDLE value) : value_(value) {}
  ~ScopedKernelHandle() {
    if (value_)
      CloseHandle(value_);
  }
  ScopedKernelHandle(const ScopedKernelHandle &) = delete;
  ScopedKernelHandle &operator=(const ScopedKernelHandle &) = delete;
  explicit operator bool() const { return value_ != nullptr; }

private:
  HANDLE value_ = nullptr;
};

bool SameSession(const runtime::Correlation &left,
                 const runtime::Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation &&
         left.session_id == right.session_id &&
         left.session_generation == right.session_generation;
}

bool IsSuccessfulEmptyComposition(const runtime::Composition &composition) {
  return composition.handled && composition.preedit.empty() &&
         composition.commit.empty() && composition.commit_preview.empty() &&
         composition.candidates.empty() &&
         composition.preview_candidates.empty() &&
         composition.highlighted_index == 0 && composition.page_index == 0 &&
         composition.page_size == 0 && composition.preedit_sel_start == 0 &&
         composition.preedit_sel_end == 0 &&
         composition.preedit_cursor_pos == 0;
}

uint32_t SpecialKey(WPARAM key, LPARAM key_data) {
  const bool extended = (key_data & (1ll << 24)) != 0;
  const uint32_t scan_code = static_cast<uint32_t>((key_data >> 16) & 0xff);
  switch (key) {
  case VK_BACK:
    return kRimeBackSpace;
  case VK_TAB:
    return kRimeTab;
  case VK_RETURN:
    return extended ? kRimeKeypadEnter : kRimeReturn;
  case VK_ESCAPE:
    return kRimeEscape;
  case VK_HOME:
    return kRimeHome;
  case VK_LEFT:
    return kRimeLeft;
  case VK_UP:
    return kRimeUp;
  case VK_RIGHT:
    return kRimeRight;
  case VK_DOWN:
    return kRimeDown;
  case VK_PRIOR:
    return kRimePageUp;
  case VK_NEXT:
    return kRimePageDown;
  case VK_END:
    return kRimeEnd;
  case VK_INSERT:
    return kRimeInsert;
  case VK_DELETE:
    return kRimeDelete;
  case VK_SHIFT:
    return scan_code == 0x36 ? kRimeShiftRight : kRimeShiftLeft;
  case VK_LSHIFT:
    return kRimeShiftLeft;
  case VK_RSHIFT:
    return kRimeShiftRight;
  case VK_CONTROL:
    return extended ? kRimeControlRight : kRimeControlLeft;
  case VK_LCONTROL:
    return kRimeControlLeft;
  case VK_RCONTROL:
    return kRimeControlRight;
  case VK_MENU:
    return extended ? kRimeAltRight : kRimeAltLeft;
  case VK_LMENU:
    return kRimeAltLeft;
  case VK_RMENU:
    return kRimeAltRight;
  case VK_CAPITAL:
    return kRimeCapsLock;
  default:
    if (key >= VK_F1 && key <= VK_F24)
      return kRimeF1 + static_cast<uint32_t>(key - VK_F1);
    return 0;
  }
}

uint32_t CharacterKey(WPARAM key, LPARAM key_data,
                      bool preserve_keyboard_state) {
  BYTE keyboard[256]{};
  if (!GetKeyboardState(keyboard))
    return 0;
  keyboard[VK_CONTROL] = 0;
  keyboard[VK_LCONTROL] = 0;
  keyboard[VK_RCONTROL] = 0;
  keyboard[VK_MENU] = 0;
  keyboard[VK_LMENU] = 0;
  keyboard[VK_RMENU] = 0;
  wchar_t text[4]{};
  const int count = ToUnicodeEx(
      static_cast<UINT>(key), static_cast<UINT>((key_data >> 16) & 0xff),
      keyboard, text, static_cast<int>(std::size(text)),
      preserve_keyboard_state ? kToUnicodeNoStateChange : 0,
      GetKeyboardLayout(0));
  return count == 1 ? static_cast<uint32_t>(text[0]) : 0;
}

} // namespace

HostKey TextService::MakeKey(WPARAM key, LPARAM key_data, bool down,
                             bool preserve_keyboard_state) const {
  uint32_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) < 0)
    modifiers |= 1;
  if ((GetKeyState(VK_CAPITAL) & 1) != 0)
    modifiers |= 2;
  if (GetKeyState(VK_CONTROL) < 0)
    modifiers |= 4;
  if (GetKeyState(VK_MENU) < 0)
    modifiers |= 8;
  if (!down)
    modifiers |= kRimeReleaseMask;
  if (key == VK_CAPITAL && down)
    modifiers ^= 2;
  uint32_t rime_key = SpecialKey(key, key_data);
  if (rime_key == 0)
    rime_key = CharacterKey(key, key_data, preserve_keyboard_state);
  return {rime_key,
          static_cast<uint32_t>((key_data >> 16) & 0xff), modifiers, down,
          static_cast<uint32_t>(GetMessageTime())};
}

HRESULT TextService::ApplyRuntimeComposition(
    ContextEntry *entry, const runtime::Composition &composition,
    const std::string *commit_override) {
  if (!entry || !entry->context)
    return E_INVALIDARG;
  runtime::Composition host_composition = composition;
  if (commit_override)
    host_composition.commit = *commit_override;
  if ((composition.state_flags & runtime::kHostInlinePreedit) == 0) {
    host_composition.preedit.clear();
    host_composition.preedit_sel_start = 0;
    host_composition.preedit_sel_end = 0;
    host_composition.preedit_cursor_pos = 0;
  } else if ((composition.state_flags & runtime::kHostCandidatePreview) != 0 &&
             !composition.commit_preview.empty()) {
    host_composition.preedit = composition.commit_preview;
    host_composition.preedit_sel_start = 0;
    host_composition.preedit_sel_end =
        static_cast<uint32_t>(host_composition.preedit.size());
    host_composition.preedit_cursor_pos = host_composition.preedit_sel_end;
  }
  return entry->composition.Apply(
      entry->context.get(), client_id_, host_composition,
      static_cast<ITfCompositionSink *>(this));
}

bool TextService::ResolveCandidateCommitOverride(
    ContextEntry *entry, const runtime::DeliveryReference &reference,
    const runtime::Composition &composition,
    const std::string **commit_override) const {
  if (!entry || !commit_override)
    return false;
  *commit_override = nullptr;
  if (!entry->exact_candidate_commit)
    return true;
  if (reference.command != runtime::Command::ClearComposition ||
      reference.correlation != entry->exact_candidate_commit->correlation ||
      !IsSuccessfulEmptyComposition(composition)) {
    return false;
  }
  *commit_override = &entry->exact_candidate_commit->preedit;
  return true;
}

bool TextService::HandlePreviewSelection(
    HWND source_window,
    const runtime::PreviewSelectionRequest &selection) {
  if (!OnActivationThread() || !source_window || !IsWindow(source_window) ||
      selection.reserved != 0)
    return false;
  const runtime::PipeClientIdentity runtime_identity =
      runtime_port_.server_identity();
  ScopedKernelHandle runtime_process(
      runtime::AcquirePipeClientIdentityLease(runtime_identity));
  DWORD source_process_id = 0;
  if (!runtime_process ||
      GetWindowThreadProcessId(source_window, &source_process_id) == 0 ||
      source_process_id != runtime_identity.process_id) {
    return false;
  }
  ContextEntry *entry = nullptr;
  for (auto &owned : contexts_) {
    if (!owned->close_requested && owned->ui_state.focused &&
        SameSession(owned->state.session_identity(), selection.correlation) &&
        owned->selection_capability_sequence ==
            selection.composition_sequence &&
        runtime::SelectionCapabilityMatches(
            owned->ui_state.selection_capability,
            selection.selection_capability)) {
      entry = owned.get();
      break;
    }
  }
  if (!entry)
    return false;
  const auto correlation =
      entry->state.PlanAbsoluteCandidate(selection.composition_sequence);
  if (!correlation)
    return false;
  // Consume before crossing another fallible boundary. A fresh opaque value
  // keeps later layout/focus UiState encodable, but sequence zero makes it
  // unusable for this already-clicked composition.
  (void)RenewSelectionCapability(entry, 0);

  runtime::Frame request;
  request.command = runtime::Command::SelectCandidateAbsolute;
  request.correlation = *correlation;
  if (!runtime::EncodeAbsoluteCandidateSelection(
          selection.absolute_index, selection.composition_sequence,
          &request.payload)) {
    entry->state.CompleteUnhandled();
    return false;
  }
  return DeliverCandidateRequest(entry, std::move(request));
}

// Sends one candidate-driven request and reconciles its reply. The caller must
// already hold a plan from ContextState for this exact frame. Shared by the
// runtime's click channel and by ITfCandidateListUIElementBehavior —
// duplicating this ladder is how the quarantine/ACK contract gets broken.
bool TextService::DeliverCandidateRequest(ContextEntry *entry,
                                          runtime::Frame &&request,
                                          std::string exact_commit) {
  if (!entry)
    return false;
  if (!exact_commit.empty()) {
    if (request.command != runtime::Command::ClearComposition ||
        entry->exact_candidate_commit) {
      entry->state.CompleteUnhandled();
      return false;
    }
    entry->exact_candidate_commit.emplace(
        ExactCandidateCommit{request.correlation, std::move(exact_commit)});
  }
  DeliveryAttempt attempt = SendDelivery(entry, std::move(request));
  if (attempt.state == DeliveryAttemptState::Rejected) {
    entry->exact_candidate_commit.reset();
    entry->state.CompleteUnhandled();
    return false;
  }
  if (attempt.state == DeliveryAttemptState::PrepareUnknown) {
    entry->pending_delivery = attempt.reference;
    entry->pending_physical_key = false;
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Cancel,
                         attempt.reference);
    return false;
  }
  if (attempt.state == DeliveryAttemptState::PreparedAmbiguous) {
    entry->pending_delivery = attempt.reference;
    entry->pending_physical_key = false;
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Recover,
                         attempt.reference);
    return true;
  }
  const runtime::Frame &reply = attempt.final_reply;
  if (reply.flags != runtime::kFlagResponse ||
      reply.command != attempt.reference.command ||
      reply.correlation != attempt.reference.correlation ||
      !entry->state.AcceptReply(reply.correlation)) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    return true;
  }
  if (reply.status != runtime::Status::Ok) {
    // This is an exact terminal reply, not an ambiguous execution. Complete
    // the host plan and retain it for the normal ACK path without poisoning
    // the runtime connection; the next valid key can continue in-place.
    entry->exact_candidate_commit.reset();
    entry->state.CompleteUnhandled();
    entry->applied_delivery = attempt.reference;
    return false;
  }
  runtime::Composition composition;
  std::string error;
  if (!runtime::DecodeComposition(reply.payload, &composition, &error)) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    return true;
  }
  const std::string *commit_override = nullptr;
  if (!ResolveCandidateCommitOverride(entry, attempt.reference, composition,
                                      &commit_override)) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    return true;
  }
  if (!composition.handled) {
    entry->exact_candidate_commit.reset();
    entry->state.CompleteUnhandled();
    entry->applied_delivery = attempt.reference;
    return false;
  }
  if (FAILED(ApplyRuntimeComposition(entry, composition, commit_override))) {
    entry->pending_delivery = attempt.reference;
    entry->deferred_delivery_composition = std::move(composition);
    return true;
  }
  entry->state.ApplySucceeded(composition);
  UpdateCandidates(entry, composition);
  entry->exact_candidate_commit.reset();
  entry->applied_delivery = attempt.reference;
  return true;
}

HRESULT TextService::HandleKey(ITfContext *context, WPARAM key,
                               LPARAM key_data, bool down, bool test_only,
                               BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  if (!context || !OnActivationThread())
    return S_OK;
  ContextEntry *entry = FindContext(context);
  const bool keyboard_disabled = KeyboardDisabled(context);
  if (entry)
    ReconcileKeyboardSecurity(entry, keyboard_disabled);
  if (keyboard_disabled ||
      (entry && entry->keyboard_security !=
                    KeyboardSecurityState::Enabled)) {
    if (entry &&
        entry->keyboard_security == KeyboardSecurityState::Closing) {
      PostRecoveryWork();
    }
    return S_OK;
  }
  ApplyDeliveryResult();
  ApplySessionResult();
  entry = FindContext(context);
  if (!entry)
    return S_OK;
  ReconcileKeyboardSecurity(entry, KeyboardDisabled(context));
  if (entry->keyboard_security != KeyboardSecurityState::Enabled) {
    if (entry->keyboard_security == KeyboardSecurityState::Closing)
      PostRecoveryWork();
    return S_OK;
  }
  if (entry->close_requested)
    return S_OK;
  if (entry->delivery_quarantined)
    return S_OK;
  if (!ApplyDeferredDelivery(entry))
    return S_OK;
  if (entry->recovery_cleanup_required) {
    PostRecoveryWork();
    return S_OK;
  }
  if (entry->state.phase() != ContextPhase::Ready) {
    if (!entry->session_pending && entry->ui_state.focused)
      ScheduleSession(entry, SessionWarmupReason::Recovery);
    return S_OK;
  }
  const HostKey host_key =
      MakeKey(key, key_data, down, test_only);
  if (test_only) {
    if (entry->pending_delivery || entry->delivery_work_pending ||
        entry->state.pending_sequence() != 0) {
      return S_OK;
    }
    *eaten = entry->state.TestKey(host_key) ? TRUE : FALSE;
    return S_OK;
  }

  const KeyPlan plan = entry->state.PlanKey(host_key);
  if (!plan.sends_request())
    return S_OK;
  runtime::Frame request;
  request.command = runtime::Command::ProcessKey;
  request.correlation = plan.correlation;
  const bool encoded = runtime::EncodeKeyEvent(plan.key, &request.payload);
  if (!encoded) {
    RecoverConnection();
    return S_OK;
  }

  DeliveryAttempt attempt = SendDelivery(entry, std::move(request));
  ReportTiming(entry->first_key_pending ? "firstKey" : "steadyProcessKey",
               attempt.elapsed, plan.correlation, attempt.status);
  entry->first_key_pending = false;
  if (attempt.state == DeliveryAttemptState::Rejected) {
    entry->state.CompleteUnhandled();
    entry->composition.ObserveUnhandledKey(key, down);
    return S_OK;
  }
  if (attempt.state == DeliveryAttemptState::PrepareUnknown) {
    entry->pending_delivery = attempt.reference;
    entry->pending_windows_key = key;
    entry->pending_key_down = down;
    entry->pending_physical_key = true;
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Cancel,
                         attempt.reference);
    return S_OK;
  }
  if (attempt.state == DeliveryAttemptState::PreparedAmbiguous) {
    entry->pending_delivery = attempt.reference;
    entry->pending_physical_key = false;
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Recover,
                         attempt.reference);
    *eaten = TRUE;
    return S_OK;
  }
  const runtime::Frame &reply = attempt.final_reply;
  if (reply.flags != runtime::kFlagResponse ||
      reply.command != attempt.reference.command ||
      reply.correlation != attempt.reference.correlation ||
      !entry->state.AcceptReply(reply.correlation)) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    *eaten = TRUE;
    return S_OK;
  }
  if (reply.status != runtime::Status::Ok) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    *eaten = TRUE;
    return S_OK;
  }
  runtime::Composition composition;
  std::string error;
  if (!runtime::DecodeComposition(reply.payload, &composition, &error)) {
    entry->pending_delivery = attempt.reference;
    QuarantineDelivery(entry);
    *eaten = TRUE;
    return S_OK;
  }
  if (!composition.handled) {
    entry->state.CompleteUnhandled();
    entry->composition.ObserveUnhandledKey(key, down);
    entry->applied_delivery = attempt.reference;
    return S_OK;
  }
  const HRESULT applied = ApplyRuntimeComposition(entry, composition);
  if (FAILED(applied)) {
    entry->pending_delivery = attempt.reference;
    entry->deferred_delivery_composition = std::move(composition);
    *eaten = TRUE;
    return S_OK;
  }
  entry->state.ApplySucceeded(composition);
  UpdateCandidates(entry, composition);
  entry->applied_delivery = attempt.reference;
  *eaten = TRUE;
  return S_OK;
}

void TextService::RecoverConnection() {
  ContextEntry *focused = nullptr;
  {
    std::lock_guard lock(session_publication_mutex_);
    desired_session_.store(nullptr);
    session_result_.store(nullptr);
  }
  for (auto &entry : contexts_) {
    ReconcileKeyboardSecurity(entry.get(),
                              KeyboardDisabled(entry->context.get()));
    const bool security_close =
        entry->keyboard_security != KeyboardSecurityState::Enabled;
    if (entry->candidates)
      entry->candidates->End();
    entry->ui_state.show_allowed = false;
    entry->composition.ResetBehaviorState();
    PublishUiState(entry.get());
    RecoveryPlan recovery = entry->state.Fail();
    if (!security_close && recovery.commit_preedit) {
      entry->recovery_preedit = std::move(*recovery.commit_preedit);
      entry->recovery_cleanup_required = true;
    } else if (security_close) {
      entry->recovery_preedit.clear();
      entry->recovery_cleanup_required = false;
    }
    if (entry->keyboard_security == KeyboardSecurityState::Enabled &&
        entry->ui_state.focused) {
      focused = entry.get();
    }
    entry->exact_candidate_commit.reset();
    entry->session_pending = false;
    entry->pending_session = {};
  }
  runtime_port_.Poison();
  PostRecoveryWork();
  if (focused) {
    ScheduleSession(focused, SessionWarmupReason::Recovery);
  } else {
    session_disconnect_requested_.store(true);
    session_worker_epoch_.fetch_add(1);
    session_worker_epoch_.notify_one();
  }
}

void TextService::UpdateCandidates(
    ContextEntry *entry, const runtime::Composition &composition) {
  if (!entry || !entry->candidates ||
      entry->keyboard_security != KeyboardSecurityState::Enabled)
    return;
  if (!RenewSelectionCapability(entry, entry->state.displayed_sequence())) {
    entry->ui_state.show_allowed = false;
    return;
  }
  BOOL show_allowed = FALSE;
  if (FAILED(entry->candidates->Update(composition, &show_allowed))) {
    entry->candidates->End();
    show_allowed = FALSE;
  }
  entry->ui_state.show_allowed = show_allowed != FALSE;
  PublishUiState(entry);
}

HRESULT TextService::OnTestKeyDown(ITfContext *context, WPARAM key,
                                   LPARAM key_data, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return BoundaryOr<HRESULT>(S_OK, [&] {
    if (GetEnvironmentVariableA("FAMO_TEST_KEY_CALLBACK_ALLOCATION_FAILURE",
                                nullptr, 0) != 0) {
      throw std::bad_alloc();
    }
    return HandleKey(context, key, key_data, true, true, eaten);
  });
}

HRESULT TextService::OnTestKeyUp(ITfContext *context, WPARAM key,
                                 LPARAM key_data, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return BoundaryOr<HRESULT>(
      S_OK, [&] { return HandleKey(context, key, key_data, false, true, eaten); });
}

HRESULT TextService::OnKeyDown(ITfContext *context, WPARAM key,
                               LPARAM key_data, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return BoundaryOr<HRESULT>(S_OK, [&] {
    if (GetEnvironmentVariableA("FAMO_TEST_KEY_CALLBACK_ALLOCATION_FAILURE",
                                nullptr, 0) != 0) {
      throw std::bad_alloc();
    }
    return HandleKey(context, key, key_data, true, false, eaten);
  });
}

HRESULT TextService::OnKeyUp(ITfContext *context, WPARAM key,
                             LPARAM key_data, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return BoundaryOr<HRESULT>(
      S_OK,
      [&] { return HandleKey(context, key, key_data, false, false, eaten); });
}

HRESULT TextService::OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return S_OK;
}

} // namespace famo::tsf
