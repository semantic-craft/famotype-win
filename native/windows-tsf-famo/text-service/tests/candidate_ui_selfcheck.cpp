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
    *show = FALSE;
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

private:
  ~FakeUiManager() = default;
  std::atomic<ULONG> references_{1};
  famo::tsf::ComPtr<ITfUIElement> element_;
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
  CHECK(!element->begun() && !element->show_allowed());
  BOOL shown = TRUE;
  CHECK(SUCCEEDED(element->IsShown(&shown)) && shown == FALSE);
  manager->fail_end = false;
  CHECK(SUCCEEDED(element->Update(Snapshot())) && manager->begins == 3);
  CHECK(SUCCEEDED(element->Update(empty)) && manager->ends == 3);

  element->Release();
  manager->Release();
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
