#include "candidate_ui_element.h"

#include <algorithm>

#include <oleauto.h>

#include "famo_guids.h"
#include "famo_utf_conversion.h"
#include "module_state.h"

namespace famo::tsf {

CandidateUiElement::CandidateUiElement(ITfUIElementMgr *manager,
                                       ITfDocumentMgr *document)
    : manager_(manager), document_(document) {
  AddModuleObject();
}

CandidateUiElement::~CandidateUiElement() {
  End();
  RemoveModuleObject();
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
      *show_allowed = show_allowed_;
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
    }
  } else {
    result = manager_->UpdateUIElement(element_id_);
  }
  if (show_allowed)
    *show_allowed = show_allowed_;
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
  if (iid == IID_IUnknown || iid == IID_ITfUIElement ||
      iid == IID_ITfCandidateListUIElement)
    *object = static_cast<ITfCandidateListUIElement *>(this);
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
  shown_ = show;
  return S_OK;
}

HRESULT CandidateUiElement::IsShown(BOOL *show) {
  if (!show)
    return E_POINTER;
  *show = shown_;
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
}

HRESULT CandidateUiElement::GetCurrentPage(UINT *page) {
  if (!page)
    return E_POINTER;
  *page = current_page_;
  return S_OK;
}

} // namespace famo::tsf
