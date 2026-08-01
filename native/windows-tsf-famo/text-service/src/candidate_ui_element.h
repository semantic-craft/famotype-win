#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <ctffunc.h>
#include <msctf.h>

#include "com_ptr.h"
#include "famo_runtime_protocol.h"

namespace famo::tsf {

// What the application asked for through the candidate-list COM interfaces.
enum class CandidateBehavior { Select, Finalize, Abort, FinalizeExact };

// Back-channel from the UI element to the owning text service. Declared here
// rather than taking a TextService* because candidate_ui_selfcheck links
// candidate_ui_element.cpp without text_service.cpp.
class CandidateUiHost {
public:
  // The element's effective visibility changed outside of Update() — the host
  // called ITfUIElement::Show(). Republish the runtime UI state.
  virtual void OnCandidateVisibilityChanged(class CandidateUiElement *element) = 0;
  // The application selected, finalized or aborted the candidate list itself.
  // index is read only for Select; all other behaviors use engine state.
  virtual HRESULT OnCandidateBehavior(class CandidateUiElement *element,
                                      CandidateBehavior behavior,
                                      UINT index) = 0;
  // An integrated host routed a key at the candidate list instead of letting
  // it reach the key sink. Goes through the same engine path as real input.
  virtual HRESULT OnCandidateKeyDown(class CandidateUiElement *element,
                                     WPARAM key, LPARAM key_data,
                                     BOOL *eaten) = 0;

protected:
  ~CandidateUiHost() = default;
};

// Multiple inheritance gives the two interface subobjects distinct addresses,
// so QueryInterface has to cast each IID to its exact interface type.
class CandidateUiElement final : public ITfCandidateListUIElementBehavior,
                                 public ITfIntegratableCandidateListUIElement {
public:
  CandidateUiElement(ITfUIElementMgr *manager, ITfDocumentMgr *document);

  HRESULT Update(const runtime::Composition &composition,
                 BOOL *show_allowed = nullptr);
  void End();
  bool begun() const { return begun_; }
  // TSF AddRefs the element inside BeginUIElement, so it can outlive the
  // ContextEntry that owns it. The host pointer is raw and non-owning, and
  // ~ContextEntry clears it.
  void SetHost(CandidateUiHost *host) { host_ = host; }
  // Effective visibility of the element's own UI: the host both has to allow
  // it at BeginUIElement and must not have hidden it since via Show().
  BOOL visible() const { return show_allowed_ && shown_ ? TRUE : FALSE; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override;
  HRESULT STDMETHODCALLTYPE GetGUID(GUID *guid) override;
  HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
  HRESULT STDMETHODCALLTYPE IsShown(BOOL *show) override;
  HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD *flags) override;
  HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **document) override;
  HRESULT STDMETHODCALLTYPE GetCount(UINT *count) override;
  HRESULT STDMETHODCALLTYPE GetSelection(UINT *index) override;
  HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR *value) override;
  HRESULT STDMETHODCALLTYPE GetPageIndex(UINT *indices, UINT size,
                                         UINT *page_count) override;
  HRESULT STDMETHODCALLTYPE SetPageIndex(UINT *indices,
                                         UINT page_count) override;
  HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT *page) override;

  HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override;
  HRESULT STDMETHODCALLTYPE Finalize() override;
  HRESULT STDMETHODCALLTYPE Abort() override;

  HRESULT STDMETHODCALLTYPE SetIntegrationStyle(GUID style) override;
  HRESULT STDMETHODCALLTYPE GetSelectionStyle(
      TfIntegratableCandidateListSelectionStyle *style) override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(WPARAM key, LPARAM key_data,
                                      BOOL *eaten) override;
  HRESULT STDMETHODCALLTYPE ShowCandidateNumbers(BOOL *show) override;
  HRESULT STDMETHODCALLTYPE FinalizeExactCompositionString() override;

private:
  ~CandidateUiElement();
  HRESULT RequireOwnerThread() const noexcept;

  std::atomic<ULONG> references_{1};
  DWORD owner_thread_id_ = 0;
  ComPtr<ITfUIElementMgr> manager_;
  ComPtr<ITfDocumentMgr> document_;
  std::vector<std::wstring> candidates_;
  std::vector<UINT> pages_{0};
  UINT selection_ = 0;
  UINT current_page_ = 0;
  DWORD element_id_ = TF_INVALID_UIELEMENTID;
  CandidateUiHost *host_ = nullptr;
  GUID integration_style_ = GUID_NULL;
  BOOL shown_ = TRUE;
  BOOL show_allowed_ = TRUE;
  bool begun_ = false;
};

} // namespace famo::tsf
