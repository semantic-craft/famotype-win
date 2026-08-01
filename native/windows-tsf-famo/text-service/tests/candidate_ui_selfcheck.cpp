#include <atomic>
#include <cstdio>

#include "candidate_ui_element.h"
#include "famo_guids.h"

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

  int changes = 0;
  int behaviors = 0;
  famo::tsf::CandidateUiElement *last = nullptr;
  famo::tsf::CandidateBehavior last_behavior =
      famo::tsf::CandidateBehavior::Select;
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

  CHECK(behavior->SetSelection(2) == S_OK);
  CHECK(host.behaviors == 1 && host.last == host_drawn &&
        host.last_behavior == famo::tsf::CandidateBehavior::Select &&
        host.last_index == 2);
  CHECK(behavior->Finalize() == S_OK);
  CHECK(host.behaviors == 2 &&
        host.last_behavior == famo::tsf::CandidateBehavior::Finalize);
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

  // A detached element must never call back into a released host.
  const int before_detach = host.changes;
  host_drawn->SetHost(nullptr);
  CHECK(SUCCEEDED(host_drawn->Show(FALSE)));
  CHECK(host.changes == before_detach && host_drawn->visible() == FALSE);
  CHECK(behavior->SetSelection(0) == E_FAIL);
  CHECK(behavior->Finalize() == E_FAIL);
  CHECK(behavior->Abort() == E_FAIL);
  CHECK(host.behaviors == 4);
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
