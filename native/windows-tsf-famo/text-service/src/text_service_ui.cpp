#include "text_service.h"

#include <new>
#include <utility>

namespace famo::tsf {

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
    return service_->CaptureLayout(context_.get(), view_.get(), cookie);
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
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;
  ContextEntry *entry = FindContext(context);
  if (!entry)
    return S_OK;
  if (code == TF_LC_DESTROY) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return S_OK;
  }
  RefreshLayout(entry, view);
  return S_OK;
}

void TextService::RefreshLayout(ContextEntry *entry, ITfContextView *view) {
  if (!entry || !entry->context)
    return;
  ComPtr<ITfContextView> active_view(view);
  if (!active_view &&
      FAILED(entry->context->GetActiveView(active_view.put()))) {
    entry->ui_state.layout_available = false;
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
  TF_SELECTION selection{};
  ULONG fetched = 0;
  HRESULT result = entry->context->GetSelection(
      cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  RECT caret{};
  BOOL clipped = FALSE;
  if (SUCCEEDED(result) && fetched == 1 && selection.range)
    result = view->GetTextExt(cookie, selection.range, &caret, &clipped);
  if (selection.range)
    selection.range->Release();
  if (result == TS_E_NOLAYOUT || FAILED(result) || fetched != 1) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return S_OK;
  }
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  const HMONITOR monitor = MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(monitor, &monitor_info)) {
    entry->ui_state.layout_available = false;
    PublishUiState(entry);
    return S_OK;
  }
  HWND window = nullptr;
  view->GetWnd(&window);
  const UINT dpi = window ? GetDpiForWindow(window) : GetDpiForSystem();
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
  const auto correlation = entry->state.PlanUiState();
  if (!correlation)
    return;
  runtime::Frame update;
  update.command = runtime::Command::UpdateUiState;
  update.correlation = *correlation;
  std::string error;
  if (!runtime::EncodeUiState(entry->ui_state, &update.payload, &error))
    return;
  runtime_port_.Post(std::move(update));
}

} // namespace famo::tsf
