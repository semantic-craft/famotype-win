#include "text_service.h"

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

HRESULT TextService::HandleKey(ITfContext *context, WPARAM key,
                               LPARAM key_data, bool down, bool test_only,
                               BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  if (!context || !OnActivationThread())
    return S_OK;
  ApplySessionResult();
  ContextEntry *entry = FindContext(context);
  if (!entry)
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
    *eaten = entry->state.TestKey(host_key) ? TRUE : FALSE;
    return S_OK;
  }

  const KeyPlan plan = entry->state.PlanKey(host_key);
  if (!plan.sends_request())
    return S_OK;
  runtime::Frame request;
  request.correlation = plan.correlation;
  bool encoded = false;
  if (plan.request == RequestKind::ProcessKey) {
    request.command = runtime::Command::ProcessKey;
    encoded = runtime::EncodeKeyEvent(plan.key, &request.payload);
  } else {
    request.command = runtime::Command::SelectCandidate;
    encoded = runtime::EncodeCandidateIndex(plan.candidate_index,
                                            &request.payload);
  }
  if (!encoded) {
    RecoverConnection();
    return S_OK;
  }

  runtime::CallResult result =
      runtime_port_.Call(std::move(request), runtime::kHardCallDeadline);
  if (plan.request == RequestKind::ProcessKey) {
    ReportTiming(entry->first_key_pending ? "firstKey" : "steadyProcessKey",
                 result.elapsed, plan.correlation, result.status);
    entry->first_key_pending = false;
  }
  if (plan.request == RequestKind::ProcessKey &&
      result.status == runtime::Status::Unavailable &&
      runtime_port_.state() == runtime::ChannelState::Ready) {
    entry->state.CompleteUnhandled();
    entry->composition.ObserveUnhandledKey(key, down);
    return S_OK;
  }
  if (result.status != runtime::Status::Ok ||
      !entry->state.AcceptReply(result.reply.correlation)) {
    RecoverConnection();
    return S_OK;
  }
  runtime::Composition composition;
  std::string error;
  if (!runtime::DecodeComposition(result.reply.payload, &composition, &error)) {
    RecoverConnection();
    return S_OK;
  }
  if (!composition.handled) {
    entry->state.CompleteUnhandled();
    entry->composition.ObserveUnhandledKey(key, down);
    return S_OK;
  }
  runtime::Composition host_composition = composition;
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
  const HRESULT applied = entry->composition.Apply(
      context, client_id_, host_composition,
      static_cast<ITfCompositionSink *>(this));
  if (FAILED(applied)) {
    RecoverConnection();
    return S_OK;
  }
  entry->state.ApplySucceeded(composition);
  UpdateCandidates(entry, composition);
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
    if (entry->candidates)
      entry->candidates->End();
    entry->ui_state.show_allowed = false;
    entry->composition.ResetBehaviorState();
    PublishUiState(entry.get());
    RecoveryPlan recovery = entry->state.Fail();
    if (recovery.commit_preedit) {
      entry->recovery_preedit = std::move(*recovery.commit_preedit);
      entry->recovery_cleanup_required = true;
    }
    if (entry->ui_state.focused)
      focused = entry.get();
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
  if (!entry || !entry->candidates)
    return;
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
  return HandleKey(context, key, key_data, true, true, eaten);
}

HRESULT TextService::OnTestKeyUp(ITfContext *context, WPARAM key,
                                 LPARAM key_data, BOOL *eaten) {
  return HandleKey(context, key, key_data, false, true, eaten);
}

HRESULT TextService::OnKeyDown(ITfContext *context, WPARAM key,
                               LPARAM key_data, BOOL *eaten) {
  return HandleKey(context, key, key_data, true, false, eaten);
}

HRESULT TextService::OnKeyUp(ITfContext *context, WPARAM key,
                             LPARAM key_data, BOOL *eaten) {
  return HandleKey(context, key, key_data, false, false, eaten);
}

HRESULT TextService::OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  return S_OK;
}

} // namespace famo::tsf
