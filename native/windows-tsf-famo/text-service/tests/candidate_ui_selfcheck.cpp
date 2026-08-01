#include <atomic>
#include <cstdio>
#include <thread>

#include "candidate_ui_element.h"
#include "famo_guids.h"

#include <ctffunc.h>
#include <oleauto.h>

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #value, __FILE__,   \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

namespace {

class FakeUiManager final : public ITfUIElementMgr {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfUIElementMgr)
      *object = static_cast<ITfUIElementMgr *>(this);
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
  HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement *element, BOOL *show,
                                           DWORD *id) override {
    if (!element || !show || !id)
      return E_POINTER;
    ++begins;
    *show = allow_show ? TRUE : FALSE;
    *id = 42;
    element_.reset(element);
    element->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
    if (id != 42)
      return E_INVALIDARG;
    ++updates;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
    if (id != 42)
      return E_INVALIDARG;
    ++ends;
    element_.reset();
    return fail_end ? E_FAIL : S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetUIElement(DWORD id,
                                         ITfUIElement **element) override {
    if (!element)
      return E_POINTER;
    *element = nullptr;
    if (id != 42 || !element_)
      return E_INVALIDARG;
    *element = element_.get();
    (*element)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements **enumerator) override {
    if (!enumerator)
      return E_POINTER;
    *enumerator = nullptr;
    return E_NOTIMPL;
  }

  int begins = 0;
  int updates = 0;
  int ends = 0;
  bool fail_end = false;
  bool allow_show = false;

private:
  ~FakeUiManager() = default;
  std::atomic<ULONG> references_{1};
  famo::tsf::ComPtr<ITfUIElement> element_;
};

class FakeCandidateHost final : public famo::tsf::CandidateUiHost {
public:
  void OnCandidateVisibilityChanged(
      famo::tsf::CandidateUiElement *element) override {
    ++changes;
    last = element;
  }

  HRESULT OnCandidateBehavior(famo::tsf::CandidateUiElement *element,
                              famo::tsf::CandidateBehavior behavior,
                              UINT index) override {
    ++behaviors;
    last = element;
    last_behavior = behavior;
    last_index = index;
    return result;
  }

  HRESULT OnCandidateKeyDown(famo::tsf::CandidateUiElement *element,
                             WPARAM key, LPARAM, BOOL *eaten) override {
    ++keys;
    last = element;
    last_key = key;
    if (eaten)
      *eaten = key_eaten;
    return result;
  }

  int changes = 0;
  int behaviors = 0;
  int keys = 0;
  WPARAM last_key = 0;
  bool key_eaten = true;
  famo::tsf::CandidateUiElement *last = nullptr;
  famo::tsf::CandidateBehavior last_behavior =
      famo::tsf::CandidateBehavior::Finalize;
  UINT last_index = 0;
  HRESULT result = S_OK;
};

famo::runtime::Composition Snapshot() {
  famo::runtime::Composition value;
  value.candidates = {{"\xe4\xbd\xa0", "", "1", 0, 0},
                      {"\xe5\xb0\xbc", "", "2", 0, 0},
                      {"\xe6\xb3\xa5", "", "3", 0, 0}};
  value.highlighted_index = 1;
  value.page_size = 3;
  return value;
}

int RunChecks() {
  auto *manager = new FakeUiManager();
  auto *element = new famo::tsf::CandidateUiElement(manager, nullptr);
  BOOL allowed = TRUE;
  CHECK(SUCCEEDED(element->Update(Snapshot(), &allowed)));
  CHECK(element->begun() && !allowed && manager->begins == 1);

  DWORD flags = 0;
  UINT count = 0;
  UINT selection = 0;
  CHECK(SUCCEEDED(element->GetUpdatedFlags(&flags)));
  CHECK((flags & TF_CLUIE_STRING) != 0);
  CHECK(SUCCEEDED(element->GetCount(&count)) && count == 3);
  CHECK(SUCCEEDED(element->GetSelection(&selection)) && selection == 1);
  BSTR candidate = nullptr;
  CHECK(SUCCEEDED(element->GetString(1, &candidate)));
  CHECK(candidate && std::wstring_view(candidate) == L"\u5c3c");
  SysFreeString(candidate);

  UINT page_count = 0;
  UINT pages[1]{};
  CHECK(SUCCEEDED(element->GetPageIndex(pages, 1, &page_count)));
  CHECK(page_count == 1 && pages[0] == 0);
  CHECK(SUCCEEDED(element->Update(Snapshot())) && manager->updates == 1);
  famo::runtime::Composition empty;
  CHECK(SUCCEEDED(element->Update(empty)) && manager->ends == 1);
  CHECK(!element->begun());

  CHECK(SUCCEEDED(element->Update(Snapshot())) && manager->begins == 2);
  manager->fail_end = true;
  CHECK(SUCCEEDED(element->Update(empty)) && manager->ends == 2);
  CHECK(!element->begun() && !element->visible());
  BOOL shown = TRUE;
  CHECK(SUCCEEDED(element->IsShown(&shown)) && shown == FALSE);
  manager->fail_end = false;
  CHECK(SUCCEEDED(element->Update(Snapshot())) && manager->begins == 3);
  CHECK(SUCCEEDED(element->Update(empty)) && manager->ends == 3);

  element->Release();
  manager->Release();

  // A host that allows the TIP's own UI at BeginUIElement may still take the
  // drawing over later, by calling ITfUIElement::Show(FALSE). The element must
  // report the combined status so the runtime window follows the host.
  auto *host_drawn_manager = new FakeUiManager();
  host_drawn_manager->allow_show = true;
  auto *host_drawn =
      new famo::tsf::CandidateUiElement(host_drawn_manager, nullptr);
  FakeCandidateHost host;
  host_drawn->SetHost(&host);
  BOOL host_allowed = FALSE;
  CHECK(SUCCEEDED(host_drawn->Update(Snapshot(), &host_allowed)));
  CHECK(host_allowed == TRUE && host_drawn->visible() == TRUE);
  CHECK(SUCCEEDED(host_drawn->Show(FALSE)));
  CHECK(host_drawn->visible() == FALSE);
  BOOL host_shown = TRUE;
  CHECK(SUCCEEDED(host_drawn->IsShown(&host_shown)) && host_shown == FALSE);
  // The host is told once, and told which element changed, so the runtime
  // window can follow without waiting for the next composition update.
  CHECK(host.changes == 1 && host.last == host_drawn);
  // Repeating the same request must not re-notify.
  CHECK(SUCCEEDED(host_drawn->Show(FALSE)) && host.changes == 1);
  // Restoring visibility notifies again.
  CHECK(SUCCEEDED(host_drawn->Show(TRUE)));
  CHECK(host.changes == 2 && host_drawn->visible() == TRUE);
  // While hidden, Update() keeps reporting the effective status.
  CHECK(SUCCEEDED(host_drawn->Show(FALSE)));
  BOOL while_hidden = TRUE;
  CHECK(SUCCEEDED(host_drawn->Update(Snapshot(), &while_hidden)));
  CHECK(while_hidden == FALSE);
  // A fresh element session clears the previous host's hide.
  famo::runtime::Composition none;
  CHECK(SUCCEEDED(host_drawn->Update(none)) && !host_drawn->begun());
  BOOL reborn = FALSE;
  CHECK(SUCCEEDED(host_drawn->Update(Snapshot(), &reborn)) && reborn == TRUE);

  // A host that draws the candidates itself drives selection, commit and
  // cancel through ITfCandidateListUIElementBehavior.
  famo::tsf::ComPtr<ITfCandidateListUIElementBehavior> behavior;
  CHECK(SUCCEEDED(host_drawn->QueryInterface(
      IID_ITfCandidateListUIElementBehavior,
      reinterpret_cast<void **>(behavior.put()))));
  CHECK(behavior.get() == static_cast<ITfCandidateListUIElement *>(host_drawn));

  const int before_selection_updates = host_drawn_manager->updates;
  CHECK(behavior->SetSelection(2) == S_OK);
  CHECK(SUCCEEDED(behavior->GetSelection(&selection)) && selection == 1);
  // SetSelection is an engine request. The element must publish only the
  // selection returned by the runtime, never speculative local state.
  CHECK(host.behaviors == 1 &&
        host.last_behavior == famo::tsf::CandidateBehavior::Select &&
        host.last_index == 2 &&
        host_drawn_manager->updates == before_selection_updates);
  famo::runtime::Composition selected_snapshot = Snapshot();
  selected_snapshot.highlighted_index = 2;
  CHECK(SUCCEEDED(host_drawn->Update(selected_snapshot)));
  CHECK(SUCCEEDED(behavior->GetSelection(&selection)) && selection == 2);
  CHECK(behavior->Finalize() == S_OK);
  CHECK(host.behaviors == 2 && host.last == host_drawn &&
        host.last_behavior == famo::tsf::CandidateBehavior::Finalize &&
        host.last_index == 2);
  CHECK(behavior->Abort() == S_OK);
  CHECK(host.behaviors == 3 &&
        host.last_behavior == famo::tsf::CandidateBehavior::Abort);
  // The host's verdict is the method's result.
  host.result = E_FAIL;
  CHECK(behavior->Finalize() == E_FAIL && host.behaviors == 4);
  host.result = S_OK;
  // An index outside the published page never reaches the runtime session.
  CHECK(behavior->SetSelection(3) == E_INVALIDARG && host.behaviors == 4);
  // Nor does one against a list the element has already ended.
  famo::runtime::Composition cleared;
  CHECK(SUCCEEDED(host_drawn->Update(cleared)) && !host_drawn->begun());
  CHECK(behavior->SetSelection(0) == E_INVALIDARG && host.behaviors == 4);
  CHECK(SUCCEEDED(host_drawn->Update(Snapshot())));

  // An integrated host — a search box — additionally drives keyboarding and
  // presentation through ITfIntegratableCandidateListUIElement.
  famo::tsf::ComPtr<ITfIntegratableCandidateListUIElement> integratable;
  CHECK(SUCCEEDED(host_drawn->QueryInterface(
      IID_ITfIntegratableCandidateListUIElement,
      reinterpret_cast<void **>(integratable.put()))));
  // Multiple inheritance gives these two interfaces distinct addresses, so
  // QueryInterface must cast each IID to its exact interface type.
  CHECK(static_cast<void *>(integratable.get()) !=
        static_cast<void *>(behavior.get()));

  CHECK(integratable->SetIntegrationStyle(famo::tsf::kIntegrationStyleSearchBox) ==
        S_OK);
  // An unknown style is refused rather than silently accepted.
  CHECK(integratable->SetIntegrationStyle(GUID_NULL) == E_NOTIMPL);

  TfIntegratableCandidateListSelectionStyle style = STYLE_IMPLIED_SELECTION;
  CHECK(SUCCEEDED(integratable->GetSelectionStyle(&style)));
  // The highlighted candidate is the one that commits, so the selection is
  // active rather than merely implied.
  CHECK(style == STYLE_ACTIVE_SELECTION);

  BOOL numbers = FALSE;
  CHECK(SUCCEEDED(integratable->ShowCandidateNumbers(&numbers)));
  // GetString carries the candidate text only, so the host has to number.
  CHECK(numbers == TRUE);

  // Keys route through the same engine path as physical input.
  BOOL eaten = FALSE;
  const int before_keys = host.keys;
  CHECK(SUCCEEDED(integratable->OnKeyDown('2', 0, &eaten)));
  CHECK(host.keys == before_keys + 1 && host.last_key == '2' && eaten == TRUE);

  // Exact finalization is distinct from converting the highlighted candidate.
  const int before_finalize = host.behaviors;
  CHECK(integratable->FinalizeExactCompositionString() == S_OK);
  CHECK(host.behaviors == before_finalize + 1 &&
        host.last_behavior == famo::tsf::CandidateBehavior::FinalizeExact);

  CHECK(integratable->GetSelectionStyle(nullptr) == E_POINTER);
  CHECK(integratable->ShowCandidateNumbers(nullptr) == E_POINTER);
  CHECK(integratable->OnKeyDown('2', 0, nullptr) == E_POINTER);

  // TSF owns this element on the activation thread. Every Behavior and
  // Integratable entry point must reject a direct cross-thread call before it
  // reads or mutates element state or reaches the host callback.
  HRESULT wrong_thread_results[8]{};
  TfIntegratableCandidateListSelectionStyle wrong_thread_style =
      STYLE_ACTIVE_SELECTION;
  BOOL wrong_thread_eaten = TRUE;
  BOOL wrong_thread_numbers = FALSE;
  const int behaviors_before_wrong_thread = host.behaviors;
  const int keys_before_wrong_thread = host.keys;
  std::thread wrong_thread([&] {
    wrong_thread_results[0] = behavior->SetSelection(0);
    wrong_thread_results[1] = behavior->Finalize();
    wrong_thread_results[2] = behavior->Abort();
    wrong_thread_results[3] =
        integratable->SetIntegrationStyle(
            famo::tsf::kIntegrationStyleSearchBox);
    wrong_thread_results[4] =
        integratable->GetSelectionStyle(&wrong_thread_style);
    wrong_thread_results[5] =
        integratable->OnKeyDown('2', 0, &wrong_thread_eaten);
    wrong_thread_results[6] =
        integratable->ShowCandidateNumbers(&wrong_thread_numbers);
    wrong_thread_results[7] =
        integratable->FinalizeExactCompositionString();
  });
  wrong_thread.join();
  for (HRESULT result : wrong_thread_results)
    CHECK(result == RPC_E_WRONG_THREAD);
  CHECK(wrong_thread_eaten == TRUE && wrong_thread_numbers == FALSE);
  CHECK(host.behaviors == behaviors_before_wrong_thread &&
        host.keys == keys_before_wrong_thread);

  // A host may retain the COM element after EndUIElement. Methods that would
  // reach the live context must reject that stale element; Abort stays locally
  // idempotent without calling the still-active text service.
  CHECK(SUCCEEDED(host_drawn->Update(cleared)) && !host_drawn->begun());
  const int behaviors_before_end = host.behaviors;
  const int keys_before_end = host.keys;
  CHECK(behavior->Finalize() == E_FAIL);
  CHECK(behavior->Abort() == S_OK);
  CHECK(integratable->FinalizeExactCompositionString() == E_FAIL);
  BOOL ended_eaten = TRUE;
  CHECK(integratable->OnKeyDown('2', 0, &ended_eaten) == E_FAIL);
  CHECK(ended_eaten == FALSE);
  CHECK(host.behaviors == behaviors_before_end && host.keys == keys_before_end);
  CHECK(SUCCEEDED(host_drawn->Update(Snapshot())) && host_drawn->begun());

  // A detached element must never call back into a released host.
  const int before_detach = host.changes;
  const int behaviors_before_detach = host.behaviors;
  const int keys_before_detach = host.keys;
  host_drawn->SetHost(nullptr);
  CHECK(SUCCEEDED(host_drawn->Show(FALSE)));
  CHECK(host.changes == before_detach && host_drawn->visible() == FALSE);
  CHECK(behavior->SetSelection(0) == E_FAIL);
  CHECK(behavior->Finalize() == E_FAIL);
  CHECK(behavior->Abort() == E_FAIL);
  CHECK(integratable->FinalizeExactCompositionString() == E_FAIL);
  BOOL detached_eaten = TRUE;
  CHECK(integratable->OnKeyDown('2', 0, &detached_eaten) == E_FAIL);
  CHECK(detached_eaten == FALSE);
  // Styles are element-local, so they keep answering after detach.
  CHECK(SUCCEEDED(integratable->GetSelectionStyle(&style)));
  CHECK(host.behaviors == behaviors_before_detach &&
        host.keys == keys_before_detach);
  integratable.reset();
  behavior.reset();
  host_drawn->Release();
  host_drawn_manager->Release();

  std::printf("candidate_ui_selfcheck: OK\n");
  return 0;
}

} // namespace

int main() {
  try {
    return RunChecks();
  } catch (...) {
    std::fprintf(stderr, "candidate_ui_selfcheck: unexpected exception\n");
    return 1;
  }
}
