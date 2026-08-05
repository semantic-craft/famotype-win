#include "text_service.h"

#include <limits>
#include <new>
#include <utility>

#include "abi_boundary.h"

namespace famo::tsf {
namespace {


runtime::UiRect ToUiRect(const RECT &rect) {
  return {rect.left, rect.top, rect.right, rect.bottom};
}

bool RectanglesOverlap(const RECT &left, const RECT &right) {
  return left.left <= right.right && left.right >= right.left &&
         left.top <= right.bottom && left.bottom >= right.top;
}

bool ConvertToPhysicalCoordinates(HWND window,
                                  DPI_AWARENESS_CONTEXT awareness,
                                  RECT *rect) {
  if (!window || !awareness || !rect)
    return false;
  if (AreDpiAwarenessContextsEqual(
          awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) ||
      AreDpiAwarenessContextsEqual(
          awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    return true;
  }
  POINT corners[2] = {{rect->left, rect->top},
                      {rect->right, rect->bottom}};
  if (!LogicalToPhysicalPointForPerMonitorDPI(window, &corners[0]) ||
      !LogicalToPhysicalPointForPerMonitorDPI(window, &corners[1])) {
    return false;
  }
  *rect = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  return true;
}

UINT PhysicalDpiForWindow(HWND window, DPI_AWARENESS_CONTEXT awareness) {
  if (!window)
    return GetDpiForSystem();
  const UINT reported = GetDpiForWindow(window);
  if (awareness &&
      (AreDpiAwarenessContextsEqual(awareness,
                                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) ||
       AreDpiAwarenessContextsEqual(
           awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))) {
    return reported;
  }
  POINT scale[2] = {{0, 0}, {96, 0}};
  if (LogicalToPhysicalPointForPerMonitorDPI(window, &scale[0]) &&
      LogicalToPhysicalPointForPerMonitorDPI(window, &scale[1])) {
    const int64_t physical_span = static_cast<int64_t>(scale[1].x) - scale[0].x;
    const int64_t baseline = reported == 0 ? 96 : reported;
    const int64_t physical_dpi = (baseline * physical_span + 48) / 96;
    if (physical_dpi > 0 &&
        physical_dpi <=
            static_cast<int64_t>((std::numeric_limits<UINT>::max)()))
      return static_cast<UINT>(physical_dpi);
  }
  return reported;
}

class PhysicalCoordinateScope {
public:
  PhysicalCoordinateScope()
      : previous_(SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~PhysicalCoordinateScope() {
    if (previous_)
      SetThreadDpiAwarenessContext(previous_);
  }
  explicit operator bool() const { return previous_ != nullptr; }

private:
  DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

} // namespace

class TextService::LayoutEditSession final : public ITfEditSession {
public:
  LayoutEditSession(TextService *service, ITfContext *context,
                    ITfContextView *view)
      : service_(service), context_(context), view_(view) {
    service_->AddRef();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfEditSession)
      *object = static_cast<ITfEditSession *>(this);
    if (!*object)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    return ComBoundary(
        [&] { return service_->CaptureLayout(context_.get(), view_.get(), cookie); });
  }

private:
  ~LayoutEditSession() { service_->Release(); }
  std::atomic<ULONG> references_{1};
  TextService *service_;
  ComPtr<ITfContext> context_;
  ComPtr<ITfContextView> view_;
};

HRESULT TextService::OnLayoutChange(ITfContext *context, TfLayoutCode code,
                                    ITfContextView *view) {
  return ComBoundary([&] {
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;
  ContextEntry *entry = FindContext(context);
  if (!entry)
    return S_OK;
  if (code == TF_LC_DESTROY) {
    entry->ui_state.layout_available = false;
    entry->candidate_owner = nullptr;
    PublishUiState(entry);
    return S_OK;
  }
  RefreshLayout(entry, view);
  return S_OK;
  });
}

void TextService::OnCandidateVisibilityChanged(CandidateUiElement *element) {
  // The host called ITfUIElement::Show() to take over or release the drawing
  // of this element. Republish so the runtime window follows immediately.
  if (!element || !OnActivationThread())
    return;
  for (auto &owned : contexts_) {
    if (!owned || owned->candidates.get() != element)
      continue;
    owned->ui_state.show_allowed = element->visible() != FALSE;
    PublishUiState(owned.get());
    return;
  }
}

TextService::ContextEntry *
TextService::FindContextByCandidateElement(CandidateUiElement *element) {
  if (!element)
    return nullptr;
  for (auto &owned : contexts_) {
    if (owned && !owned->close_requested &&
        owned->keyboard_security == KeyboardSecurityState::Enabled &&
        owned->candidates.get() == element) {
      return owned.get();
    }
  }
  return nullptr;
}

HRESULT TextService::OnCandidateKeyDown(CandidateUiElement *element, WPARAM key,
                                        LPARAM key_data, BOOL *eaten) {
  if (!eaten)
    return E_POINTER;
  *eaten = FALSE;
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;
  ContextEntry *entry = FindContextByCandidateElement(element);
  if (!entry || !entry->context)
    return E_FAIL;
  // The integrated host is routing real input at the candidate list, not
  // asking for a second keyboard model, so this is the physical key path.
  return HandleKey(entry->context.get(), key, key_data, /*down=*/true,
                   /*test_only=*/false, eaten);
}

HRESULT TextService::OnCandidateBehavior(CandidateUiElement *element,
                                         CandidateBehavior behavior,
                                         UINT index) {
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;
  ContextEntry *entry = FindContextByCandidateElement(element);
  if (!entry)
    return E_FAIL;
  std::string exact_commit;
  if (behavior == CandidateBehavior::FinalizeExact) {
    // Copy before reserving a sequence: allocation failure must not strand the
    // ContextState with a request that was never sent.
    exact_commit = ExactCompositionText(entry->state.displayed());
    if (exact_commit.empty())
      return E_FAIL;
  }
  // The click channel's capability token authenticates an unsigned cross
  // process WM_COPYDATA; an in-process COM call on the activation thread does
  // not need it, and consuming it here would disable the runtime's own preview
  // click for the same composition. The plan is the gate: it rejects a
  // not-Ready phase, an in-flight request and a context with no composition.
  const auto correlation =
      entry->state.PlanAbsoluteCandidate(entry->state.displayed_sequence());
  if (!correlation)
    return E_FAIL;
  runtime::Frame request;
  request.correlation = *correlation;
  switch (behavior) {
  case CandidateBehavior::Select:
    request.command = runtime::Command::HighlightCandidate;
    if (!runtime::EncodeCandidateIndex(index, &request.payload)) {
      entry->state.CompleteUnhandled();
      return E_INVALIDARG;
    }
    break;
  case CandidateBehavior::Finalize:
    request.command = runtime::Command::CommitComposition;
    break;
  case CandidateBehavior::Abort:
    request.command = runtime::Command::ClearComposition;
    break;
  case CandidateBehavior::FinalizeExact:
    // Clear the engine through the normal transaction ladder. The successful
    // empty reply is applied with the captured displayed preedit as a host-only
    // commit override; no new wire command is needed.
    request.command = runtime::Command::ClearComposition;
    break;
  }
  return DeliverCandidateRequest(entry, std::move(request),
                                 std::move(exact_commit))
             ? S_OK
             : E_FAIL;
}

void TextService::RefreshLayout(ContextEntry *entry, ITfContextView *view) {
  if (!entry || !entry->context)
    return;
  ComPtr<ITfContextView> active_view(view);
  if (!active_view &&
      FAILED(entry->context->GetActiveView(active_view.put()))) {
    entry->ui_state.layout_available = false;
    entry->candidate_owner = nullptr;
    PublishUiState(entry);
    return;
  }
  auto *session = new (std::nothrow)
      LayoutEditSession(this, entry->context.get(), active_view.get());
  if (!session) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return;
  }
  HRESULT edit_result = E_FAIL;
  const HRESULT requested = entry->context->RequestEditSession(
      client_id_, session, TF_ES_ASYNC | TF_ES_READ, &edit_result);
  session->Release();
  if (FAILED(requested) || FAILED(edit_result)) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
  }
}

HRESULT TextService::CaptureLayout(ITfContext *context, ITfContextView *view,
                                   TfEditCookie cookie) {
  if (!context || !view)
    return E_INVALIDARG;
  ContextEntry *entry = FindContext(context);
  if (!entry)
    return S_OK;
  ComPtr<ITfRange> layout_caret;
  TfAnchor caret_edge = TF_ANCHOR_END;
  HRESULT result = entry->composition.CloneLayoutCaret(
      cookie, entry->context.get(), layout_caret.put(), &caret_edge);
  RECT caret{};
  RECT view_bounds{};
  BOOL clipped = FALSE;
  if (SUCCEEDED(result))
    result = view->GetTextExt(cookie, layout_caret.get(), &caret, &clipped);
  if (SUCCEEDED(result)) {
    // The measured extent covers the character the caret sits against; collapse
    // it back to the caret's own edge so placement is unchanged.
    if (caret_edge == TF_ANCHOR_END)
      caret.left = caret.right;
    else
      caret.right = caret.left;
  }
  if (SUCCEEDED(result))
    result = view->GetScreenExt(&view_bounds);
  if (result == TS_E_NOLAYOUT || FAILED(result)) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return S_OK;
  }

  HWND window = nullptr;
  if (FAILED(view->GetWnd(&window)))
    window = nullptr;
  entry->candidate_owner = window;
  DPI_AWARENESS_CONTEXT awareness = nullptr;
  if (window) {
    awareness = GetWindowDpiAwarenessContext(window);
    RECT window_bounds{};
    const HWND root = GetAncestor(window, GA_ROOT);
    const HWND evidence_window = root ? root : window;
    if (!awareness || !GetWindowRect(evidence_window, &window_bounds) ||
        !ConvertToPhysicalCoordinates(window, awareness, &caret) ||
        !ConvertToPhysicalCoordinates(window, awareness, &view_bounds) ||
        !ConvertToPhysicalCoordinates(window, awareness, &window_bounds) ||
        !RectanglesOverlap(view_bounds, window_bounds)) {
      entry->ui_state.layout_available = false;
      PublishUiState(entry);
      return S_OK;
    }
  }

  const std::optional<runtime::UiRect> normalized = NormalizeLayoutCaret(
      ToUiRect(caret), ToUiRect(view_bounds), clipped != FALSE);
  if (!normalized) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return S_OK;
  }

  caret = {normalized->left, normalized->top, normalized->right,
           normalized->bottom};
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  {
    PhysicalCoordinateScope physical_coordinates;
    const HMONITOR monitor =
        physical_coordinates
            ? MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST)
            : nullptr;
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info)) {
      entry->ui_state.layout_available = false;
      PublishUiState(entry);
      return S_OK;
    }
  }
  const UINT dpi = PhysicalDpiForWindow(window, awareness);
  entry->ui_state.caret = {caret.left, caret.top, caret.right, caret.bottom};
  const RECT &work = monitor_info.rcWork;
  entry->ui_state.work_area = {work.left, work.top, work.right, work.bottom};
  entry->ui_state.dpi = dpi == 0 ? 96 : dpi;
  entry->ui_state.layout_available = true;
  PublishUiState(entry);
  return S_OK;
}

void TextService::SetFocused(ContextEntry *entry, bool focused) {
  if (!entry)
    return;
  if (!focused) {
    std::lock_guard lock(session_publication_mutex_);
    const std::shared_ptr<const SessionWarmupResult> result =
        session_result_.load();
    if (entry->session_pending && result &&
        result->identity == entry->pending_session) {
      session_result_.store(nullptr);
      if (result->ready &&
          result->identity.activation_generation == activation_generation_ &&
          result->identity.connection_generation == connection_generation_ &&
          runtime_port_.state() == runtime::ChannelState::Ready) {
        entry->state.Open(result->identity);
        entry->first_key_pending = true;
      }
    }
    const std::shared_ptr<const runtime::Correlation> desired =
        desired_session_.load();
    if (entry->session_pending && desired &&
        *desired == entry->pending_session) {
      desired_session_.store(nullptr);
    }
    entry->session_pending = false;
    entry->pending_session = {};
    session_retry_wake_.notify_all();
  }
  if (entry->ui_state.focused == focused)
    return;
  entry->composition.ResetBehaviorState();
  entry->ui_state.focused = focused;
  PublishUiState(entry);
}

void TextService::PublishUiState(ContextEntry *entry) {
  if (!entry || runtime_port_.state() != runtime::ChannelState::Ready)
    return;
  const auto correlation =
      entry->keyboard_security == KeyboardSecurityState::Enabled
          ? entry->state.PlanUiState()
          : entry->state.PlanSecurityUiState();
  if (!correlation)
    return;
  runtime::Frame update;
  update.command = runtime::Command::UpdateUiState;
  update.correlation = *correlation;
  runtime::UiState published = entry->ui_state;
  if (entry->keyboard_security != KeyboardSecurityState::Enabled) {
    published.focused = false;
    published.show_allowed = false;
  }
  try {
    auto snapshot = std::make_shared<runtime::RuntimeSnapshot>();
    snapshot->correlation = *correlation;
    snapshot->composition = entry->state.displayed();
    snapshot->ui_state = published;
    snapshot->source_window =
        reinterpret_cast<uintptr_t>(entry->candidate_owner);
    snapshot->style = runtime_style_.load();
    snapshot->selection_target = recovery_window_;
    snapshot->require_in_process_owner = true;
    snapshot->composition_sequence = entry->state.displayed_sequence();
    snapshot->ui_sequence = correlation->sequence;
    snapshot->revision = ++candidate_revision_;
    candidate_window_.Publish(std::move(snapshot));
  } catch (...) {
    // Candidate presentation is best effort and never changes input delivery.
  }
  // The in-process presenter draws the candidate so the popup can be owned by
  // ITfContextView::GetWnd, and the Runtime must not put a second one on
  // screen. It can only do that when this context actually has a usable owner
  // window; when it does not, muting the Runtime unconditionally would leave
  // nobody drawing at all. The host's own refusal is carried in show_allowed
  // and still suppresses both, so a host that takes the candidate UI over
  // never competes with a Famo popup.
  const bool presented_in_process =
      entry->candidate_owner != nullptr && IsWindow(entry->candidate_owner);
  if (presented_in_process)
    published.show_allowed = false;
  std::string error;
  if (!runtime::EncodeUiState(published, &update.payload, &error))
    return;
  runtime_port_.Post(std::move(update));
}

} // namespace famo::tsf
