#include "composition_controller.h"

#include <atomic>
#include <utility>

#include "famo_commit_behavior.h"
#include "famo_utf_conversion.h"
#include "module_state.h"

namespace famo::tsf {

namespace {

HRESULT CurrentSelection(TfEditCookie cookie, ITfContext *context,
                         ComPtr<ITfRange> *range) {
  TF_SELECTION selection{};
  ULONG fetched = 0;
  const HRESULT result = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                               &selection, &fetched);
  if (FAILED(result) || fetched != 1 || !selection.range)
    return FAILED(result) ? result : E_FAIL;
  range->reset(selection.range);
  return S_OK;
}

HRESULT SelectRangeEnd(TfEditCookie cookie, ITfContext *context,
                       ITfRange *range) {
  ComPtr<ITfRange> caret;
  HRESULT result = range->Clone(caret.put());
  if (FAILED(result))
    return result;
  result = caret->Collapse(cookie, TF_ANCHOR_END);
  if (FAILED(result))
    return result;
  TF_SELECTION selection{};
  selection.range = caret.get();
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  return context->SetSelection(cookie, 1, &selection);
}

HRESULT ReplaceRange(TfEditCookie cookie, ITfContext *context, ITfRange *range,
                     std::wstring_view text) {
  const HRESULT result =
      range->SetText(cookie, 0, text.data(), static_cast<LONG>(text.size()));
  if (FAILED(result))
    return result;
  // Once SetText succeeds the original key must stay eaten. Some CUAS/EDIT
  // hosts reject the best-effort selection sync; letting that HRESULT escape
  // would pass the same key through after its text was already committed.
  (void)SelectRangeEnd(cookie, context, range);
  return S_OK;
}

struct NeighborValue {
  bool known = false;
  wchar_t value = 0;
};

NeighborValue Neighbor(TfEditCookie cookie, ITfRange *source, bool before) {
  ComPtr<ITfRange> range;
  if (FAILED(source->Clone(range.put())))
    return {};
  if (FAILED(range->Collapse(cookie, before ? TF_ANCHOR_START : TF_ANCHOR_END)))
    return {};
  LONG moved = 0;
  const HRESULT shifted = before
                              ? range->ShiftStart(cookie, -1, &moved, nullptr)
                              : range->ShiftEnd(cookie, 1, &moved, nullptr);
  if (FAILED(shifted) || moved == 0)
    return {};
  wchar_t value = 0;
  ULONG fetched = 0;
  if (FAILED(range->GetText(cookie, 0, &value, 1, &fetched)) || fetched != 1)
    return {};
  return {true, value};
}

} // namespace

class ApplyEditSession final : public ITfEditSession {
public:
  ApplyEditSession(CompositionController *controller, ITfContext *context,
                   CompositionPlan plan, ITfCompositionSink *sink)
      : controller_(controller), context_(context), plan_(std::move(plan)),
        sink_(sink) {
    AddModuleObject();
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
    result_ =
        controller_->ApplyInSession(cookie, context_.get(), plan_, sink_.get());
    return result_;
  }
  HRESULT result() const { return result_; }

private:
  ~ApplyEditSession() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  CompositionController *controller_;
  ComPtr<ITfContext> context_;
  CompositionPlan plan_;
  ComPtr<ITfCompositionSink> sink_;
  HRESULT result_ = E_UNEXPECTED;
};

class EndEditSession final : public ITfEditSession {
public:
  explicit EndEditSession(CompositionController *controller)
      : controller_(controller) {
    AddModuleObject();
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
    result_ = controller_->EndInSession(cookie);
    return result_;
  }
  HRESULT result() const { return result_; }

private:
  ~EndEditSession() { RemoveModuleObject(); }
  std::atomic<ULONG> references_{1};
  CompositionController *controller_;
  HRESULT result_ = E_UNEXPECTED;
};

HRESULT CompositionController::Apply(ITfContext *context, TfClientId client_id,
                                     const runtime::Composition &composition,
                                     ITfCompositionSink *sink) {
  if (!context)
    return E_INVALIDARG;
  CompositionPlan plan;
  if (!Utf8ToUtf16(composition.commit, &plan.commit) ||
      !Utf8ToUtf16(composition.preedit, &plan.preedit))
    return E_INVALIDARG;
  plan.behavior_flags = composition.state_flags;
  auto *session =
      new (std::nothrow) ApplyEditSession(this, context, std::move(plan), sink);
  if (!session)
    return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT request = context->RequestEditSession(
      client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
  const HRESULT applied = session->result();
  session->Release();
  if (FAILED(request))
    return request;
  if (FAILED(session_result))
    return session_result;
  return applied;
}

HRESULT CompositionController::Recover(ITfContext *context,
                                       TfClientId client_id,
                                       std::string_view confirmed_preedit,
                                       ITfCompositionSink *sink) {
  runtime::Composition recovery;
  recovery.handled = true;
  recovery.commit.assign(confirmed_preedit);
  return Apply(context, client_id, recovery, sink);
}

HRESULT CompositionController::End(ITfContext *context, TfClientId client_id) {
  if (!context || !composition_)
    return S_OK;
  auto *session = new (std::nothrow) EndEditSession(this);
  if (!session)
    return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT request = context->RequestEditSession(
      client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
  const HRESULT ended = session->result();
  session->Release();
  if (FAILED(request))
    return request;
  if (FAILED(session_result))
    return session_result;
  return ended;
}

HRESULT CompositionController::ApplyInSession(TfEditCookie cookie,
                                              ITfContext *context,
                                              const CompositionPlan &plan,
                                              ITfCompositionSink *sink) {
  ComPtr<ITfRange> selection;
  HRESULT result = CurrentSelection(cookie, context, &selection);
  if (FAILED(result))
    return result;

  if (!plan.commit.empty()) {
    ComPtr<ITfRange> target;
    if (composition_) {
      result = composition_->GetRange(target.put());
      if (FAILED(result))
        return result;
    } else {
      result = selection->Clone(target.put());
      if (FAILED(result))
        return result;
    }
    const NeighborValue before = Neighbor(cookie, target.get(), true);
    const NeighborValue after = Neighbor(cookie, target.get(), false);
    const CommitBehaviorResult transformed = TransformCommit(
        plan.commit, before.known ? before.value : previous_commit_,
        after.known ? after.value : pending_close_, plan.behavior_flags);
    if (transformed.advance_over_existing) {
      if (composition_) {
        result = EndCurrent(cookie);
        if (FAILED(result))
          return result;
      }
      if (InjectArrow(VK_RIGHT, 1).moved == 1) {
        pending_close_ = 0;
        previous_commit_ = plan.commit.front();
        result = S_OK;
      } else {
        result = CurrentSelection(cookie, context, &selection);
        if (SUCCEEDED(result))
          result = ReplaceRange(cookie, context, selection.get(), plan.commit);
        if (SUCCEEDED(result)) {
          pending_close_ = 0;
          previous_commit_ = plan.commit.back();
        }
      }
    } else {
      result = ReplaceRange(cookie, context, target.get(), transformed.text);
      if (SUCCEEDED(result)) {
        bool can_inject = true;
        if (composition_)
          can_inject = SUCCEEDED(EndCurrent(cookie));
        // SetText already succeeded, so later composition/caret failures must
        // degrade to a plain committed insert instead of leaking the key.
        result = S_OK;
        pending_close_ = 0;
        previous_commit_ =
            transformed.text.empty() ? 0 : transformed.text.back();
        if (can_inject && transformed.caret_back > 0 &&
            InjectArrow(VK_LEFT, transformed.caret_back).moved ==
                transformed.caret_back) {
          pending_close_ = transformed.text.back();
          const size_t caret = transformed.text.size() - transformed.caret_back;
          previous_commit_ = caret == 0 ? 0 : transformed.text[caret - 1];
        }
      }
    }
    if (FAILED(result))
      return result;
    result = CurrentSelection(cookie, context, &selection);
    if (FAILED(result))
      return S_OK;
  }

  if (plan.preedit.empty()) {
    if (composition_) {
      ComPtr<ITfRange> range;
      result = composition_->GetRange(range.put());
      if (FAILED(result))
        return result;
      result = ReplaceRange(cookie, context, range.get(), L"");
      if (FAILED(result))
        return result;
      result = EndCurrent(cookie);
      if (FAILED(result))
        return result;
    }
    return S_OK;
  }

  if (!composition_) {
    ComPtr<ITfContextComposition> service;
    result = context->QueryInterface(IID_ITfContextComposition,
                                     reinterpret_cast<void **>(service.put()));
    if (FAILED(result))
      return result;
    result = service->StartComposition(cookie, selection.get(), sink,
                                       composition_.put());
    if (FAILED(result) || !composition_)
      return FAILED(result) ? result : E_FAIL;
  }

  ComPtr<ITfRange> range;
  result = composition_->GetRange(range.put());
  if (FAILED(result))
    return result;
  return ReplaceRange(cookie, context, range.get(), plan.preedit);
}

ArrowInjectionResult CompositionController::InjectArrow(WORD key,
                                                        uint32_t count) {
  const InputInjectionApi api{
      [](int value, void *) noexcept { return GetAsyncKeyState(value); },
      [](const INPUT &value, void *) noexcept {
        INPUT copy = value;
        return SendInput(1, &copy, sizeof(copy));
      }};
  injected_guard_until_ = GetTickCount64() + 200;
  const ArrowInjectionResult result = InjectArrowKeys(key, count, api);
  if (result.moved == 0)
    injected_guard_until_ = 0;
  return result;
}

void CompositionController::ObserveUnhandledKey(WPARAM key, bool down) {
  if (!down)
    return;
  if ((key == VK_LEFT || key == VK_RIGHT) &&
      GetTickCount64() < injected_guard_until_)
    return;
  switch (key) {
  case VK_SHIFT:
  case VK_LSHIFT:
  case VK_RSHIFT:
  case VK_CONTROL:
  case VK_LCONTROL:
  case VK_RCONTROL:
  case VK_MENU:
  case VK_LMENU:
  case VK_RMENU:
  case VK_CAPITAL:
    return;
  default:
    ResetBehaviorState();
  }
}

void CompositionController::ResetBehaviorState() {
  pending_close_ = 0;
  previous_commit_ = 0;
  injected_guard_until_ = 0;
}

HRESULT CompositionController::EndInSession(TfEditCookie cookie) {
  if (!composition_)
    return S_OK;
  return EndCurrent(cookie);
}

HRESULT CompositionController::EndCurrent(TfEditCookie cookie) {
  internal_end_ = true;
  const HRESULT result = composition_->EndComposition(cookie);
  internal_end_ = false;
  if (SUCCEEDED(result))
    composition_.reset();
  return result;
}

bool CompositionController::CompositionTerminated(ITfComposition *composition) {
  if (!SameComObject(composition_.get(), composition))
    return false;
  const bool external = !internal_end_;
  composition_.reset();
  return external;
}

} // namespace famo::tsf
