#include "candidate_ui_element.h"

#include <algorithm>

#include <oleauto.h>

#include "abi_boundary.h"
#include "famo_guids.h"
#include "famo_utf_conversion.h"
#include "module_state.h"

namespace famo::tsf {

CandidateUiElement::CandidateUiElement(ITfUIElementMgr *manager,
                                       ITfDocumentMgr *document)
    : owner_thread_id_(GetCurrentThreadId()), manager_(manager),
      document_(document) {
  AddModuleObject();
}

CandidateUiElement::~CandidateUiElement() {
  End();
  RemoveModuleObject();
}

HRESULT CandidateUiElement::RequireOwnerThread() const noexcept {
  return GetCurrentThreadId() == owner_thread_id_ ? S_OK : RPC_E_WRONG_THREAD;
}

HRESULT CandidateUiElement::Update(const runtime::Composition &composition,
                                   BOOL *show_allowed) {
  std::vector<std::wstring> converted;
  converted.reserve(composition.candidates.size());
  for (const auto &candidate : composition.candidates) {
    std::wstring value;
    if (!Utf8ToUtf16(candidate.text, &value))
      return E_INVALIDARG;
    converted.push_back(std::move(value));
  }

  if (converted.empty()) {
    candidates_.clear();
    selection_ = 0;
    End();
    if (show_allowed)
      *show_allowed = visible();
    return S_OK;
  }

  candidates_ = std::move(converted);
  selection_ = std::min<UINT>(composition.highlighted_index,
                              static_cast<UINT>(candidates_.size() - 1));
  pages_ = {0};
  current_page_ = 0;
  if (!manager_)
    return E_UNEXPECTED;
  HRESULT result = S_OK;
  if (!begun_) {
    BOOL allowed = TRUE;
    result = manager_->BeginUIElement(this, &allowed, &element_id_);
    if (SUCCEEDED(result)) {
      begun_ = true;
      show_allowed_ = allowed;
      // Fresh element session: any hide the host requested for the previous
      // element does not carry over.
      shown_ = TRUE;
      // A UI-less host does not inspect candidate contents in BeginUIElement;
      // FALSE means it starts reading them at UpdateUIElement. Publish that
      // first complete update immediately instead of waiting for another key.
      if (!allowed)
        result = manager_->UpdateUIElement(element_id_);
    }
  } else {
    // UpdateUIElement carries no pbShow, so the BeginUIElement answer stands
    // for the life of the element. Show() is how the host changes its mind.
    result = manager_->UpdateUIElement(element_id_);
  }
  if (show_allowed)
    *show_allowed = visible();
  return result;
}

void CandidateUiElement::End() {
  if (begun_ && manager_) {
    const HRESULT ended = manager_->EndUIElement(element_id_);
    if (FAILED(ended)) {
      show_allowed_ = FALSE;
      shown_ = FALSE;
    }
  }
  begun_ = false;
  element_id_ = TF_INVALID_UIELEMENTID;
}

HRESULT CandidateUiElement::QueryInterface(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  // Behavior carries the ITfUIElement/ITfCandidateListUIElement chain, so one
  // cast serves those IIDs. Integratable has a distinct interface-subobject
  // address and therefore needs its own exact cast.
  if (iid == IID_IUnknown || iid == IID_ITfUIElement ||
      iid == IID_ITfCandidateListUIElement ||
      iid == IID_ITfCandidateListUIElementBehavior)
    *object = static_cast<ITfCandidateListUIElementBehavior *>(this);
  else if (iid == IID_ITfIntegratableCandidateListUIElement)
    *object = static_cast<ITfIntegratableCandidateListUIElement *>(this);
  if (!*object)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

ULONG CandidateUiElement::AddRef() { return ++references_; }

ULONG CandidateUiElement::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0)
    delete this;
  return remaining;
}

HRESULT CandidateUiElement::GetDescription(BSTR *description) {
  if (!description)
    return E_POINTER;
  *description = SysAllocString(kProfileName);
  return *description ? S_OK : E_OUTOFMEMORY;
}

HRESULT CandidateUiElement::GetGUID(GUID *guid) {
  if (!guid)
    return E_POINTER;
  *guid = kCandidateUiGuid;
  return S_OK;
}

HRESULT CandidateUiElement::Show(BOOL show) {
  const BOOL requested = show ? TRUE : FALSE;
  if (shown_ == requested)
    return S_OK;
  shown_ = requested;
  // UILess contract option 1: move the element to the hide status and keep it
  // alive so the host can keep drawing from Update notifications. The host is
  // told so the runtime window follows immediately rather than at the next
  // composition update.
  if (host_)
    host_->OnCandidateVisibilityChanged(this);
  return S_OK;
}

HRESULT CandidateUiElement::IsShown(BOOL *show) {
  if (!show)
    return E_POINTER;
  *show = visible();
  return S_OK;
}

HRESULT CandidateUiElement::GetUpdatedFlags(DWORD *flags) {
  if (!flags)
    return E_POINTER;
  *flags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
           TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
  return S_OK;
}

HRESULT CandidateUiElement::GetDocumentMgr(ITfDocumentMgr **document) {
  if (!document)
    return E_POINTER;
  *document = document_.get();
  if (*document)
    (*document)->AddRef();
  return S_OK;
}

HRESULT CandidateUiElement::GetCount(UINT *count) {
  if (!count)
    return E_POINTER;
  *count = static_cast<UINT>(candidates_.size());
  return S_OK;
}

HRESULT CandidateUiElement::GetSelection(UINT *index) {
  if (!index)
    return E_POINTER;
  *index = selection_;
  return S_OK;
}

HRESULT CandidateUiElement::GetString(UINT index, BSTR *value) {
  if (!value)
    return E_POINTER;
  *value = nullptr;
  if (index >= candidates_.size())
    return E_INVALIDARG;
  *value = SysAllocStringLen(candidates_[index].data(),
                             static_cast<UINT>(candidates_[index].size()));
  return *value ? S_OK : E_OUTOFMEMORY;
}

HRESULT CandidateUiElement::GetPageIndex(UINT *indices, UINT size,
                                         UINT *page_count) {
  if (!page_count || (size != 0 && !indices))
    return E_POINTER;
  *page_count = static_cast<UINT>(pages_.size());
  const UINT copied = std::min(size, *page_count);
  std::copy_n(pages_.begin(), copied, indices);
  return copied == *page_count ? S_OK : S_FALSE;
}

HRESULT CandidateUiElement::SetPageIndex(UINT *indices, UINT page_count) {
  return ComBoundary([&] {
    if (page_count == 0 || !indices)
      return E_INVALIDARG;
    if (indices[0] != 0)
      return E_INVALIDARG;
    for (UINT index = 1; index < page_count; ++index) {
      if (indices[index] <= indices[index - 1] ||
          indices[index] >= candidates_.size())
        return E_INVALIDARG;
    }
    pages_.assign(indices, indices + page_count);
    current_page_ = std::min(current_page_, page_count - 1);
    return S_OK;
  });
}

HRESULT CandidateUiElement::GetCurrentPage(UINT *page) {
  if (!page)
    return E_POINTER;
  *page = current_page_;
  return S_OK;
}

HRESULT CandidateUiElement::SetSelection(UINT index) {
  return ComBoundary([&] {
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    // The element publishes exactly the engine's current page, so the host's
    // index is page-relative and has to land inside it.
    if (index >= candidates_.size())
      return E_INVALIDARG;
    if (!host_ || !begun_)
      return E_FAIL;
    if (selection_ == index)
      return S_OK;
    // The engine owns candidate state. Do not publish the requested index
    // until its Runtime reply reaches Update().
    return host_->OnCandidateBehavior(this, CandidateBehavior::Select, index);
  });
}

HRESULT CandidateUiElement::Finalize() {
  return ComBoundary([&] {
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    if (!host_ || !begun_)
      return E_FAIL;
    return host_->OnCandidateBehavior(this, CandidateBehavior::Finalize,
                                      selection_);
  });
}

HRESULT CandidateUiElement::Abort() {
  return ComBoundary([&] {
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    if (!host_)
      return E_FAIL;
    // Abort is intentionally idempotent. Once EndUIElement has run there is
    // no live composition to cancel, so succeed locally without reaching the
    // still-active text service through a stale element.
    if (!begun_)
      return S_OK;
    return host_->OnCandidateBehavior(this, CandidateBehavior::Abort, 0);
  });
}

HRESULT CandidateUiElement::SetIntegrationStyle(GUID style) {
  return ComBoundary([&] {
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    // Search box is the only style Windows defines. Refusing the rest keeps
    // the host from assuming an integration Famo has not been told how to
    // honour.
    if (!IsEqualGUID(style, kIntegrationStyleSearchBox))
      return E_NOTIMPL;
    integration_style_ = style;
    return S_OK;
  });
}

HRESULT CandidateUiElement::GetSelectionStyle(
    TfIntegratableCandidateListSelectionStyle *style) {
  return ComBoundary([&] {
    if (!style)
      return E_POINTER;
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    // The engine always carries a highlighted candidate on the current page,
    // and that candidate is exactly what a commit would produce, so the
    // selection is active rather than a default the user has not landed on.
    *style = STYLE_ACTIVE_SELECTION;
    return S_OK;
  });
}

HRESULT CandidateUiElement::OnKeyDown(WPARAM key, LPARAM key_data,
                                      BOOL *eaten) {
  return ComBoundary([&] {
    if (!eaten)
      return E_POINTER;
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    *eaten = FALSE;
    if (!host_ || !begun_)
      return E_FAIL;
    return host_->OnCandidateKeyDown(this, key, key_data, eaten);
  });
}

HRESULT CandidateUiElement::ShowCandidateNumbers(BOOL *show) {
  return ComBoundary([&] {
    if (!show)
      return E_POINTER;
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    // GetString publishes candidate text only; the engine's own labels never
    // reach the host, so it has to draw the numbers for selection to be usable.
    *show = TRUE;
    return S_OK;
  });
}

HRESULT CandidateUiElement::FinalizeExactCompositionString() {
  return ComBoundary([&] {
    const HRESULT thread = RequireOwnerThread();
    if (FAILED(thread))
      return thread;
    if (!host_ || !begun_)
      return E_FAIL;
    return host_->OnCandidateBehavior(
        this, CandidateBehavior::FinalizeExact, 0);
  });
}

} // namespace famo::tsf
