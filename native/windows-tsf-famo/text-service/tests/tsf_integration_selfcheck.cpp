#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>

#include <ctffunc.h>
#include <msctf.h>
#include <windows.h>

#include "com_ptr.h"
#include "fake_text_store.h"
#include "famo_guids.h"
#include "tsf_integration_support.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,       \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

namespace {

using famo::tsf::ComPtr;
using famo::tsf::test::FakeTextStore;
using famo::tsf::test::RuntimeProcess;
using famo::tsf::test::TextServiceModule;

class ScopedCom {
public:
  ScopedCom() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ScopedCom() {
    if (SUCCEEDED(result_))
      CoUninitialize();
  }
  HRESULT result() const { return result_; }

private:
  HRESULT result_;
};

class ScopedEnvironment {
public:
  ScopedEnvironment(const char *name, const char *value) : name_(name) {
    _putenv_s(name, value);
  }
  ~ScopedEnvironment() { _putenv_s(name_.c_str(), ""); }

private:
  std::string name_;
};

class ScopedNeutralLetterKeyboardState {
public:
  ScopedNeutralLetterKeyboardState() {
    captured_ = GetKeyboardState(original_) != FALSE;
    if (!captured_)
      return;
    BYTE neutral[256]{};
    CopyMemory(neutral, original_, sizeof(neutral));
    neutral[VK_SHIFT] = 0;
    neutral[VK_LSHIFT] = 0;
    neutral[VK_RSHIFT] = 0;
    neutral[VK_CAPITAL] &= static_cast<BYTE>(~1u);
    applied_ = SetKeyboardState(neutral) != FALSE;
  }
  ~ScopedNeutralLetterKeyboardState() {
    if (applied_)
      SetKeyboardState(original_);
  }
  explicit operator bool() const { return captured_ && applied_; }

private:
  BYTE original_[256]{};
  bool captured_ = false;
  bool applied_ = false;
};

class ScopedDpiAwareness {
public:
  explicit ScopedDpiAwareness(DPI_AWARENESS_CONTEXT awareness)
      : previous_(SetThreadDpiAwarenessContext(awareness)) {}
  ~ScopedDpiAwareness() {
    if (previous_)
      SetThreadDpiAwarenessContext(previous_);
  }
  explicit operator bool() const { return previous_ != nullptr; }

private:
  DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

UINT ExpectedPhysicalDpi(HWND window) {
  const UINT reported = GetDpiForWindow(window);
  POINT scale[2] = {{0, 0}, {96, 0}};
  if (!LogicalToPhysicalPointForPerMonitorDPI(window, &scale[0]) ||
      !LogicalToPhysicalPointForPerMonitorDPI(window, &scale[1])) {
    return reported;
  }
  const int64_t span = static_cast<int64_t>(scale[1].x) - scale[0].x;
  const int64_t dpi =
      static_cast<int64_t>(reported == 0 ? 96 : reported) * span / 96;
  return dpi > 0 ? static_cast<UINT>(dpi) : reported;
}

bool TestKey(ITfKeyEventSink *sink, ITfContext *context, WPARAM key,
             bool expected_eaten) {
  BOOL eaten = FALSE;
  return SUCCEEDED(sink->OnTestKeyDown(context, key, 0, &eaten)) &&
         (eaten != FALSE) == expected_eaten;
}

bool SendKey(ITfKeyEventSink *sink, ITfContext *context, WPARAM key,
             bool expected_eaten) {
  BOOL eaten = FALSE;
  return SUCCEEDED(sink->OnKeyDown(context, key, 0, &eaten)) &&
         (eaten != FALSE) == expected_eaten;
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

struct CandidateWindowProbe {
  HWND window = nullptr;
  bool visible = false;
  // Restricts the search to the candidate owned by this window. Earlier tests
  // in the same process can leave a candidate window behind when a renderer
  // thread outlives its bounded shutdown, so "any candidate in this process"
  // is not a stable identity to assert on.
  HWND required_owner = nullptr;
};

BOOL CALLBACK FindProcessCandidateWindow(HWND window, LPARAM parameter) {
  DWORD process_id = 0;
  if (GetWindowThreadProcessId(window, &process_id) == 0 ||
      process_id != GetCurrentProcessId()) {
    return TRUE;
  }
  wchar_t class_name[64]{};
  if (GetClassNameW(window, class_name,
                    static_cast<int>(std::size(class_name))) == 0 ||
      std::wstring_view(class_name) != L"FamoRuntimeCandidateWindow") {
    return TRUE;
  }
  auto *probe = reinterpret_cast<CandidateWindowProbe *>(parameter);
  if (probe->required_owner &&
      GetWindow(window, GW_OWNER) != probe->required_owner) {
    return TRUE;
  }
  probe->window = window;
  probe->visible = IsWindowVisible(window) != FALSE;
  return probe->visible ? FALSE : TRUE;
}

CandidateWindowProbe ProbeProcessCandidateWindow(HWND required_owner) {
  CandidateWindowProbe probe;
  probe.required_owner = required_owner;
  EnumWindows(&FindProcessCandidateWindow,
              reinterpret_cast<LPARAM>(&probe));
  return probe;
}

// Waits for one specific popup to become hidden without being destroyed.
// Scanning for "some hidden candidate window" cannot express that: whichever
// text service the machine has active is loaded into this process too and
// presents its own popup on the same owner, so identity has to be asserted on
// the handle that was actually observed.
bool WaitForCandidateHidden(HWND window) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    PumpMessages();
    if (IsWindow(window) && !IsWindowVisible(window))
      return true;
    Sleep(5);
  }
  return false;
}

bool WaitForProcessCandidateVisibility(bool visible,
                                       CandidateWindowProbe *result = nullptr,
                                       HWND required_owner = nullptr) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    PumpMessages();
    CandidateWindowProbe probe = ProbeProcessCandidateWindow(required_owner);
    if (probe.window && probe.visible == visible) {
      if (result)
        *result = probe;
      return true;
    }
    Sleep(5);
  }
  return false;
}

using SessionCheck = std::function<bool(
    ITfKeyEventSink *, ITfContext *, FakeTextStore *, ITfTextInputProcessorEx *,
    ITfThreadMgr *, ITfDocumentMgr *)>;

struct TestDocument {
  ComPtr<ITfDocumentMgr> document;
  ComPtr<FakeTextStore> store;
  ComPtr<ITfContext> context;
};

bool CreateTestDocument(ITfThreadMgr *thread_manager, TfClientId client_id,
                        TestDocument *target) {
  CHECK(thread_manager && target);
  CHECK(SUCCEEDED(thread_manager->CreateDocumentMgr(target->document.put())));
  target->store.reset(new FakeTextStore());
  TfEditCookie edit_cookie = TF_INVALID_COOKIE;
  CHECK(SUCCEEDED(target->document->CreateContext(
      client_id, 0, static_cast<ITextStoreACP *>(target->store.get()),
      target->context.put(), &edit_cookie)));
  CHECK(SUCCEEDED(target->document->Push(target->context.get())));
  return true;
}

// A thread manager that delivers focus for the already-focused document from
// inside AdviseSink, the way Chromium, Electron and SearchHost text stores do.
// Everything else is the real thread manager, so the only difference from an
// ordinary activation is when the first context is created: while Activate is
// still running rather than after it returns.
class ReentrantFocusThreadMgr final : public ITfThreadMgr, public ITfSource {
public:
  ReentrantFocusThreadMgr(ITfThreadMgr *inner, ITfDocumentMgr *focused)
      : inner_(inner), focused_(focused) {
    inner_->QueryInterface(IID_ITfSource,
                           reinterpret_cast<void **>(source_.put()));
  }

  bool reentered() const { return reentered_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_ITfThreadMgr) {
      *object = static_cast<ITfThreadMgr *>(this);
    } else if (iid == IID_ITfSource) {
      *object = static_cast<ITfSource *>(this);
    } else {
      // Everything the text service acquires by QueryInterface -- the
      // keystroke manager, the UI element manager, the single-sink source --
      // comes straight from the real thread manager.
      return inner_->QueryInterface(iid, object);
    }
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

  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown *sink,
                                       DWORD *cookie) override {
    const HRESULT result = source_->AdviseSink(iid, sink, cookie);
    if (FAILED(result) || iid != IID_ITfThreadMgrEventSink)
      return result;
    ComPtr<ITfThreadMgrEventSink> thread_sink;
    if (SUCCEEDED(sink->QueryInterface(
            IID_ITfThreadMgrEventSink,
            reinterpret_cast<void **>(thread_sink.put())))) {
      thread_sink->OnSetFocus(focused_.get(), nullptr);
      reentered_ = true;
    }
    return result;
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
    return source_->UnadviseSink(cookie);
  }

  HRESULT STDMETHODCALLTYPE Activate(TfClientId *id) override {
    return inner_->Activate(id);
  }
  HRESULT STDMETHODCALLTYPE Deactivate() override {
    return inner_->Deactivate();
  }
  HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr **mgr) override {
    return inner_->CreateDocumentMgr(mgr);
  }
  HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs **mgrs)
      override {
    return inner_->EnumDocumentMgrs(mgrs);
  }
  HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr **mgr) override {
    return inner_->GetFocus(mgr);
  }
  HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr *mgr) override {
    return inner_->SetFocus(mgr);
  }
  HRESULT STDMETHODCALLTYPE AssociateFocus(HWND window, ITfDocumentMgr *mgr,
                                           ITfDocumentMgr **previous)
      override {
    return inner_->AssociateFocus(window, mgr, previous);
  }
  HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL *focus) override {
    return inner_->IsThreadFocus(focus);
  }
  HRESULT STDMETHODCALLTYPE GetFunctionProvider(
      REFCLSID clsid, ITfFunctionProvider **provider) override {
    return inner_->GetFunctionProvider(clsid, provider);
  }
  HRESULT STDMETHODCALLTYPE EnumFunctionProviders(
      IEnumTfFunctionProviders **providers) override {
    return inner_->EnumFunctionProviders(providers);
  }
  HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr **mgr)
      override {
    return inner_->GetGlobalCompartment(mgr);
  }

private:
  ~ReentrantFocusThreadMgr() = default;

  std::atomic<ULONG> references_{1};
  ComPtr<ITfThreadMgr> inner_;
  ComPtr<ITfSource> source_;
  ComPtr<ITfDocumentMgr> focused_;
  bool reentered_ = false;
};

// A context created by a synchronous OnSetFocus keeps whatever the text
// service held at that moment for its whole life. Activation therefore has to
// finish acquiring every interface a callback can need before it advises the
// first sink; when it does not, the candidate UI element is built around a
// null ITfUIElementMgr and the host never gets a candidate list even though
// keys and composition keep working.
bool ReentrantActivationFocusStillBeginsCandidates(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ReentrantFocusThreadMgr> host(new ReentrantFocusThreadMgr(
      thread_manager.get(), target.document.get()));
  host->Release();

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(
      static_cast<ITfThreadMgr *>(host.get()), client_id, service.put())));
  CHECK(host->reentered());

  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
  CHECK(SendKey(key_sink.get(), target.context.get(), 'I', true));
  CHECK(target.store->text() == L"ni");

  famo::runtime::UiState published{};
  CHECK(module->UiStateForTest(service.get(), target.context.get(),
                               &published));
  CHECK(published.show_allowed);

  key_sink.reset();
  const bool service_deactivated = SUCCEEDED(service->Deactivate());
  service.reset();
  host.reset();
  const bool document_popped = SUCCEEDED(target.document->Pop(TF_POPF_ALL));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  const bool manager_deactivated = SUCCEEDED(thread_manager->Deactivate());
  thread_manager.reset();
  const bool runtime_finished = runtime.Finish();
  return service_deactivated && document_popped && manager_deactivated &&
         runtime_finished;
}

bool RunTextStoreSession(
    TextServiceModule *module, const SessionCheck &check,
    std::chrono::steady_clock::duration *activation_elapsed = nullptr,
    bool wait_for_session = true, HWND initial_window = nullptr) {
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  target.store->set_window_for_test(initial_window);
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  const auto activation_started = std::chrono::steady_clock::now();
  const HRESULT activate_result = module->CreateForTest(
      thread_manager.get(), client_id, service.put());
  if (activation_elapsed) {
    *activation_elapsed = std::chrono::steady_clock::now() - activation_started;
  }
  if (FAILED(activate_result)) {
    std::fprintf(stderr, "TextService::Activate failed: 0x%08lx\n",
                 static_cast<unsigned long>(activate_result));
    return false;
  }
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  if (wait_for_session) {
    const auto ready_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
           std::chrono::steady_clock::now() < ready_deadline) {
      Sleep(5);
    }
    CHECK(std::chrono::steady_clock::now() < ready_deadline);
  }

  const bool scenario_passed = check(
      key_sink.get(), target.context.get(), target.store.get(), service.get(),
      thread_manager.get(), target.document.get());
  key_sink.reset();
  const bool service_deactivated = SUCCEEDED(service->Deactivate());
  service.reset();
  const bool document_popped =
      SUCCEEDED(target.document->Pop(TF_POPF_ALL));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  const bool manager_deactivated = SUCCEEDED(thread_manager->Deactivate());
  thread_manager.reset();
  return scenario_passed && service_deactivated && document_popped &&
         manager_deactivated;
}

bool ActivationPublishesOpenInputMode(TextServiceModule *module) {
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  ComPtr<ITfCompartmentMgr> compartments;
  CHECK(SUCCEEDED(thread_manager->QueryInterface(
      IID_ITfCompartmentMgr,
      reinterpret_cast<void **>(compartments.put()))));
  ComPtr<ITfCompartment> keyboard_open;
  CHECK(SUCCEEDED(compartments->GetCompartment(
      GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, keyboard_open.put())));
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_I4;
  value.lVal = 0;
  CHECK(SUCCEEDED(keyboard_open->SetValue(client_id, &value)));

  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));
  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));

  VariantClear(&value);
  CHECK(SUCCEEDED(keyboard_open->GetValue(&value)));
  const bool opened = value.vt == VT_I4 && value.lVal != 0;
  VariantClear(&value);

  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  keyboard_open.reset();
  compartments.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(opened);
  CHECK(module->CanUnload());
  return true;
}

bool HealthyRoundtrip(TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, 'N', true));
        CHECK(TestKey(key_sink, context, 'N', true));
        {
          ScopedEnvironment fail_key(
              "FAMO_TEST_KEY_CALLBACK_ALLOCATION_FAILURE", "1");
          BOOL eaten = TRUE;
          CHECK(key_sink->OnKeyDown(context, 'N', 0, &eaten) == S_OK);
          CHECK(eaten == FALSE);
          CHECK(store->text().empty());
        }
        const auto first_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'N', true));
        const auto first_elapsed = std::chrono::steady_clock::now() - first_started;
        CHECK(store->text() == L"n");
        const auto steady_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'I', true));
        const auto steady_elapsed =
            std::chrono::steady_clock::now() - steady_started;
        std::printf("tsf_timing first_key_us=%lld steady_key_us=%lld\n",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            first_elapsed)
                            .count()),
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            steady_elapsed)
                            .count()));
        CHECK(store->text() == L"ni");
        CHECK(TestKey(key_sink, context, '2', true));
        CHECK(SendKey(key_sink, context, '2', true));
        CHECK(store->text() == L"\x5c3c");

        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        CHECK(store->text() == L"\x5c3chao");
        CHECK(SendKey(key_sink, context, VK_SPACE, true));
        CHECK(store->text() == L"\x5c3c\x597d");

        BOOL eaten = TRUE;
        CHECK(SUCCEEDED(key_sink->OnTestKeyUp(context, 'N', 0, &eaten)) &&
              eaten == FALSE);
        CHECK(SUCCEEDED(key_sink->OnKeyUp(context, 'N', 0, &eaten)) &&
              eaten == FALSE);
        CHECK(TestKey(key_sink, context, VK_SHIFT, true));
        CHECK(SendKey(key_sink, context, VK_SHIFT, false));
        CHECK(SUCCEEDED(
            key_sink->OnTestKeyUp(context, VK_SHIFT, 0, &eaten)) &&
              eaten == TRUE);
        CHECK(SUCCEEDED(key_sink->OnKeyUp(context, VK_SHIFT, 0, &eaten)) &&
              eaten == TRUE);
        CHECK(TestKey(key_sink, context, VK_F12, false));
        CHECK(SendKey(key_sink, context, VK_F12, false));
        CHECK(store->text() == L"\x5c3c\x597d");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool OptionalContextSinkFailureStillTypes(TextServiceModule *module,
                                          const wchar_t *runtime_path,
                                          const char *failure_name) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  bool passed = false;
  {
    ScopedEnvironment unavailable(failure_name, "1");
    passed = RunTextStoreSession(
        module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                   FakeTextStore *store, ITfTextInputProcessorEx *,
                   ITfThreadMgr *, ITfDocumentMgr *) {
          CHECK(TestKey(key_sink, context, 'N', true));
          CHECK(SendKey(key_sink, context, 'N', true));
          CHECK(store->text() == L"n");
          return true;
        });
  }
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool CompositionLayoutUsesActiveRangeEnd(TextServiceModule *module,
                                         const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [module](ITfKeyEventSink *key_sink, ITfContext *context,
                       FakeTextStore *store, ITfTextInputProcessorEx *service,
                       ITfThreadMgr *, ITfDocumentMgr *) {
        ScopedDpiAwareness unaware(DPI_AWARENESS_CONTEXT_UNAWARE);
        CHECK(static_cast<bool>(unaware));
        HWND view_window = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0,
            1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(view_window != nullptr);
        CHECK(AreDpiAwarenessContextsEqual(
            GetWindowDpiAwarenessContext(view_window),
            DPI_AWARENESS_CONTEXT_UNAWARE));
        store->set_window_for_test(view_window);
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");

        // Model a WebView host whose document selection spans a different
        // range while the TSF composition remains active. Candidate placement
        // must follow the composition's active end, not this host selection.
        store->set_selection_for_test(0, 1, TS_AE_START);
        ComPtr<ITfTextLayoutSink> layout_sink;
        CHECK(SUCCEEDED(service->QueryInterface(
            IID_ITfTextLayoutSink,
            reinterpret_cast<void **>(layout_sink.put()))));
        const size_t before = store->text_ext_query_count();
        CHECK(SUCCEEDED(
            layout_sink->OnLayoutChange(context, TF_LC_CHANGE, nullptr)));
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (store->text_ext_query_count() == before &&
               std::chrono::steady_clock::now() < deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(store->text_ext_query_count() > before);
        CHECK(store->screen_ext_query_count() > 0);
        CHECK(store->window_query_count() > 0);
        // The layout query covers the last composed character rather than a
        // zero-length caret: a text store measures laid-out text, so an empty
        // range has nothing to report. Placement still follows the
        // composition's active end, which is this range's right edge.
        CHECK(store->last_text_ext_start() == 1);
        CHECK(store->last_text_ext_end() == 2);

        famo::runtime::UiState published{};
        CHECK(module->UiStateForTest(service, context, &published));
        CHECK(published.layout_available);
        POINT expected_corners[2] = {{1, 0}, {1, 16}};
        CHECK(LogicalToPhysicalPointForPerMonitorDPI(view_window,
                                                     &expected_corners[0]));
        CHECK(LogicalToPhysicalPointForPerMonitorDPI(view_window,
                                                     &expected_corners[1]));
        CHECK(published.caret.left == expected_corners[0].x);
        CHECK(published.caret.top == expected_corners[0].y);
        CHECK(published.caret.right == expected_corners[1].x);
        CHECK(published.caret.bottom == expected_corners[1].y);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        {
          ScopedDpiAwareness physical(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
          CHECK(static_cast<bool>(physical));
          RECT expected_caret{expected_corners[0].x, expected_corners[0].y,
                              expected_corners[1].x, expected_corners[1].y};
          const HMONITOR monitor =
              MonitorFromRect(&expected_caret, MONITOR_DEFAULTTONEAREST);
          CHECK(monitor != nullptr);
          CHECK(GetMonitorInfoW(monitor, &monitor_info));
        }
        CHECK(published.work_area.left == monitor_info.rcWork.left);
        CHECK(published.work_area.top == monitor_info.rcWork.top);
        CHECK(published.work_area.right == monitor_info.rcWork.right);
        CHECK(published.work_area.bottom == monitor_info.rcWork.bottom);
        CHECK(published.dpi == ExpectedPhysicalDpi(view_window));
        std::printf("tsf_layout_physical dpi=%u caret=%ld,%ld,%ld,%ld "
                    "work=%ld,%ld,%ld,%ld\n",
                    published.dpi, static_cast<long>(published.caret.left),
                    static_cast<long>(published.caret.top),
                    static_cast<long>(published.caret.right),
                    static_cast<long>(published.caret.bottom),
                    static_cast<long>(published.work_area.left),
                    static_cast<long>(published.work_area.top),
                    static_cast<long>(published.work_area.right),
                    static_cast<long>(published.work_area.bottom));
        store->set_window_for_test(nullptr);
        CHECK(DestroyWindow(view_window));

        {
          ScopedDpiAwareness system(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
          CHECK(static_cast<bool>(system));
          HWND system_window =
              CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC",
                              L"", WS_POPUP, 0, 0, 1024, 768, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
          CHECK(system_window != nullptr);
          CHECK(AreDpiAwarenessContextsEqual(
              GetWindowDpiAwarenessContext(system_window),
              DPI_AWARENESS_CONTEXT_SYSTEM_AWARE));
          store->set_window_for_test(system_window);
          const size_t system_before = store->text_ext_query_count();
          CHECK(SUCCEEDED(
              layout_sink->OnLayoutChange(context, TF_LC_CHANGE, nullptr)));
          const auto system_deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(1);
          while (store->text_ext_query_count() == system_before &&
                 std::chrono::steady_clock::now() < system_deadline) {
            PumpMessages();
            Sleep(5);
          }
          CHECK(store->text_ext_query_count() > system_before);
          famo::runtime::UiState system_published{};
          CHECK(module->UiStateForTest(service, context, &system_published));
          CHECK(system_published.layout_available);
          CHECK(system_published.dpi == ExpectedPhysicalDpi(system_window));
          CHECK(GetDpiForWindow(system_window) > 0);
          std::printf("tsf_layout_system_aware dpi=%u reported=%u\n",
                      system_published.dpi, GetDpiForWindow(system_window));
          store->set_window_for_test(nullptr);
          CHECK(DestroyWindow(system_window));
        }
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool CandidateWindowUsesContextViewOwner(TextServiceModule *module,
                                         const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  HWND owner = CreateWindowExW(
      0, L"STATIC", L"tsf-candidate-owner", WS_OVERLAPPEDWINDOW, 0, 0, 1024,
      768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  ShowWindow(owner, SW_SHOW);
  CHECK(SetForegroundWindow(owner));
  CHECK(GetForegroundWindow() == owner);
  const bool passed = RunTextStoreSession(
      module, [owner](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *, ITfTextInputProcessorEx *service,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        ComPtr<ITfTextLayoutSink> layout_sink;
        CHECK(SUCCEEDED(service->QueryInterface(
            IID_ITfTextLayoutSink,
            reinterpret_cast<void **>(layout_sink.put()))));
        CHECK(SUCCEEDED(
            layout_sink->OnLayoutChange(context, TF_LC_CHANGE, nullptr)));

        CandidateWindowProbe probe;
        CHECK(WaitForProcessCandidateVisibility(true, &probe));
        CHECK(GetWindow(probe.window, GW_OWNER) == owner);
        return true;
      }, nullptr, true, owner);
  CHECK(DestroyWindow(owner));
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool SearchCandidateProviderIsDiscoverable(TextServiceModule *module,
                                           const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 4, true, 0, -1, -1, -1, false,
                      false, true));
  ComPtr<ITfCandidateList> surviving_list;
  const bool passed = RunTextStoreSession(
      module, [&surviving_list](ITfKeyEventSink *, ITfContext *,
                               FakeTextStore *, ITfTextInputProcessorEx *,
                               ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        ComPtr<ITfFunctionProvider> provider;
        // The test export activates the service with the harness's application
        // client id, so TSF exposes its single sink through the reserved app
        // provider key. Production activation supplies the TIP client id and
        // indexes this same provider by the CLSID returned from GetType.
        CHECK(SUCCEEDED(thread_manager->GetFunctionProvider(
            GUID_APP_FUNCTIONPROVIDER, provider.put())));
        GUID type{};
        CHECK(SUCCEEDED(provider->GetType(&type)) &&
              type == famo::tsf::kTextServiceClsid);
        BSTR description = nullptr;
        CHECK(SUCCEEDED(provider->GetDescription(&description)) &&
              description != nullptr && SysStringLen(description) > 0);
        SysFreeString(description);

        ComPtr<ITfFnSearchCandidateProvider> search;
        CHECK(SUCCEEDED(provider->GetFunction(
            GUID_NULL, IID_ITfFnSearchCandidateProvider,
            reinterpret_cast<IUnknown **>(search.put()))));
        BSTR display_name = nullptr;
        CHECK(SUCCEEDED(search->GetDisplayName(&display_name)) &&
              display_name != nullptr && SysStringLen(display_name) > 0);
        SysFreeString(display_name);

        CHECK(search->GetSearchCandidates(nullptr, nullptr,
                                          surviving_list.put()) ==
              E_INVALIDARG);
        CHECK(!surviving_list);
        BSTR application_id = SysAllocString(L"");
        BSTR empty_query = SysAllocString(L"");
        CHECK(application_id && empty_query);
        CHECK(search->GetSearchCandidates(empty_query, application_id,
                                          surviving_list.put()) == S_FALSE);
        CHECK(!surviving_list);
        SysFreeString(empty_query);

        BSTR query = SysAllocString(L"ni");
        CHECK(query);
        CHECK(search->GetSearchCandidates(query, application_id, nullptr) ==
              E_POINTER);
        CHECK(SUCCEEDED(search->GetSearchCandidates(
            query, application_id, surviving_list.put())));
        CHECK(surviving_list);

        ULONG count = 0;
        CHECK(SUCCEEDED(surviving_list->GetCandidateNum(&count)) && count == 3);
        CHECK(surviving_list->GetCandidateNum(nullptr) == E_POINTER);
        ComPtr<ITfCandidateString> first;
        CHECK(SUCCEEDED(surviving_list->GetCandidate(0, first.put())));
        CHECK(surviving_list->GetCandidate(0, nullptr) == E_INVALIDARG);
        ULONG index = ULONG_MAX;
        CHECK(SUCCEEDED(first->GetIndex(&index)) && index == 0);
        BSTR first_text = nullptr;
        CHECK(SUCCEEDED(first->GetString(&first_text)) && first_text &&
              std::wstring_view(first_text, SysStringLen(first_text)) ==
                  L"\u4f60");
        SysFreeString(first_text);
        CHECK(surviving_list->GetCandidate(count, first.put()) == E_FAIL);
        CHECK(!first);

        ComPtr<IEnumTfCandidates> enumerator;
        CHECK(SUCCEEDED(
            surviving_list->EnumCandidates(enumerator.put())));
        ComPtr<ITfCandidateString> enumerated;
        ULONG fetched = 0;
        CHECK(enumerator->Next(1, enumerated.put(), nullptr) == S_OK);
        CHECK(SUCCEEDED(enumerated->GetIndex(&index)) && index == 0);
        enumerated.reset();
        CHECK(SUCCEEDED(enumerator->Reset()));
        CHECK(enumerator->Next(1, nullptr, &fetched) == E_INVALIDARG);
        CHECK(enumerator->Next(1, enumerated.put(), &fetched) == S_OK &&
              fetched == 1);
        CHECK(SUCCEEDED(enumerated->GetIndex(&index)) && index == 0);
        ComPtr<IEnumTfCandidates> clone;
        CHECK(SUCCEEDED(enumerator->Clone(clone.put())));
        CHECK(clone->Skip(1) == S_OK);
        enumerated.reset();
        fetched = 0;
        CHECK(clone->Next(2, enumerated.put(), &fetched) == S_FALSE &&
              fetched == 1);
        CHECK(SUCCEEDED(enumerated->GetIndex(&index)) && index == 2);
        CHECK(SUCCEEDED(enumerator->Reset()));
        CHECK(enumerator->Skip(count + 1) == S_FALSE);

        CHECK(SUCCEEDED(surviving_list->SetResult(0, CAND_SELECTED)));
        CHECK(SUCCEEDED(surviving_list->SetResult(ULONG_MAX, CAND_CANCELED)));
        CHECK(surviving_list->SetResult(count, CAND_FINALIZED) ==
              E_INVALIDARG);
        CHECK(surviving_list->SetResult(
                  0, static_cast<TfCandidateResult>(99)) == E_INVALIDARG);
        BSTR result = SysAllocString(L"\u4f60");
        CHECK(result);
        CHECK(search->SetResult(query, application_id, result) == E_NOTIMPL);
        SysFreeString(result);
        SysFreeString(query);
        SysFreeString(application_id);
        return true;
      });
  CHECK(passed);
  CHECK(surviving_list);
  CHECK(!module->CanUnload());
  ULONG count = 0;
  CHECK(SUCCEEDED(surviving_list->GetCandidateNum(&count)) && count == 3);
  surviving_list.reset();
  CHECK(module->CanUnload());
  const bool runtime_finished = runtime.Finish();
  return runtime_finished;
}

bool AllocationBoundariesReleaseReferences(TextServiceModule *module) {
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  {
    ScopedEnvironment fail_activation(
        "FAMO_TEST_ACTIVATION_ALLOCATION_FAILURE", "1");
    ComPtr<ITfTextInputProcessorEx> rejected;
    CHECK(module->CreateForTest(thread_manager.get(), client_id,
                                rejected.put()) == E_OUTOFMEMORY);
    CHECK(!rejected);
  }
  CHECK(module->CanUnload());

  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));
  {
    ScopedEnvironment fail_context_insert(
        "FAMO_TEST_CONTEXT_INSERT_ALLOCATION_FAILURE", "1");
    ComPtr<ITfTextInputProcessorEx> rejected;
    CHECK(module->CreateForTest(thread_manager.get(), client_id,
                                rejected.put()) == E_OUTOFMEMORY);
    CHECK(!rejected);
  }
  CHECK(module->CanUnload());
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  {
    ScopedEnvironment fail_deactivate(
        "FAMO_TEST_DEACTIVATE_ALLOCATION_FAILURE", "1");
    CHECK(service->Deactivate() == E_OUTOFMEMORY);
  }
  service.reset();
  CHECK(module->CanUnload());
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return true;
}

bool ForcedDeactivationAbandonsTerminalDelivery(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"copy-failure", 0, 2, true, 0, 1));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink,
      reinterpret_cast<void **>(thread_sink.put()))));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  const auto second_ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < second_ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < second_ready_deadline);

  {
    ScopedEnvironment pause_recovery(
        "FAMO_TEST_PAUSE_DELIVERY_RECOVERY", "1");
    CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(first.document.get(), second.document.get())));
    CHECK(SendKey(key_sink.get(), first.context.get(), 'N', true));
    CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(second.document.get(), first.document.get())));
    CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));
    CHECK(first.store->text().empty());
    CHECK(second.store->text().empty());
    ScopedEnvironment fail_deactivate(
        "FAMO_TEST_DEACTIVATE_ALLOCATION_FAILURE", "1");
    CHECK(service->Deactivate() == E_OUTOFMEMORY);
  }

  thread_sink.reset();
  key_sink.reset();
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(module->CanUnload());
  CHECK(runtime.Finish());
  return true;
}

bool ForcedDeactivationAttemptsUnavailableEpochOnce(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"no-reply", 0, 1));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink,
      reinterpret_cast<void **>(thread_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  {
    ScopedEnvironment pause_recovery(
        "FAMO_TEST_PAUSE_DELIVERY_RECOVERY", "1");
    CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(first.document.get(), second.document.get())));
    CHECK(SendKey(key_sink.get(), first.context.get(), 'N', false));
    CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(second.document.get(), first.document.get())));
    CHECK(SendKey(key_sink.get(), second.context.get(), 'N', false));
    ScopedEnvironment fail_deactivate(
        "FAMO_TEST_DEACTIVATE_ALLOCATION_FAILURE", "1");
    CHECK(service->Deactivate() == E_OUTOFMEMORY);
  }
  CHECK(module->TerminalCleanupConnectAttemptsForTest() == 1);

  thread_sink.reset();
  key_sink.reset();
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(module->CanUnload());
  CHECK(runtime.Finish());
  return true;
}

bool ForcedDeactivationAbandonsIdleContexts(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, true, 0, 1, 0, 0));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink,
      reinterpret_cast<void **>(thread_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  {
    ScopedEnvironment fail_deactivate(
        "FAMO_TEST_DEACTIVATE_ALLOCATION_FAILURE", "1");
    CHECK(service->Deactivate() == E_OUTOFMEMORY);
  }
  CHECK(module->TerminalCleanupConnectAttemptsForTest() == 1);

  thread_sink.reset();
  key_sink.reset();
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(module->CanUnload());
  CHECK(runtime.Finish());
  return true;
}

bool TerminalPublicationSlotSerializesFailures(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"copy-failure-sticky", 0, 1, true, 0,
                      2));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink,
      reinterpret_cast<void **>(thread_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  {
    ScopedEnvironment pause_recovery(
        "FAMO_TEST_PAUSE_DELIVERY_RECOVERY", "1");
    CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(first.document.get(), second.document.get())));
    CHECK(SendKey(key_sink.get(), first.context.get(), 'N', true));
    CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(second.document.get(), first.document.get())));
    CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));
  }

  // Do not pump the recovery window until the first terminal result occupies
  // the allocation-free slot and the single worker reaches the second one.
  Sleep(200);
  for (int attempt = 0; attempt < 100; ++attempt) {
    PumpMessages();
    Sleep(5);
  }

  thread_sink.reset();
  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(module->CanUnload());
  // Exactly two per-session abandons prove neither terminal publication was
  // overwritten and no connection-wide cleanup was needed at deactivation.
  CHECK(runtime.Finish());
  return true;
}

// Turning inline preedit off is a preference about what the user reads, not
// permission to leave the host with no composition. A text store can only
// measure text it holds, so a composition-less host answers GetTextExt with
// nothing usable and the candidate window loses its anchor. The composition is
// therefore maintained either way; the preference only decides what the
// candidate window itself draws.
bool DisabledInlinePreeditStillComposesInHost(TextServiceModule *module,
                                              const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, false));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");
        CHECK(SendKey(key_sink, context, '2', true));
        CHECK(store->text() == L"\x5c3c");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool InlinePreeditPreservesUtf16Selection(TextServiceModule *module,
                                          const wchar_t *runtime_path) {
  ScopedEnvironment offsets("FAMO_TEST_PREEDIT_OFFSETS", "1");
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"abcd");
        CHECK(store->selection().acpStart == 2 &&
              store->selection().acpEnd == 2);

        // The engine marks 1..4 as the segment being converted, but the host
        // selection is the caret, not the segment: a host that is told the
        // selection spans several characters treats the composition as a
        // range selection and measures it as one. Only the cursor offset
        // crosses over, converted from bytes to UTF-16 like every other case
        // here.
        CHECK(SendKey(key_sink, context, 'B', true));
        CHECK(store->text() == L"abcdef");
        CHECK(store->selection().acpStart == 4 &&
              store->selection().acpEnd == 4);

        CHECK(SendKey(key_sink, context, 'C', true));
        CHECK(store->text() == L"\u4f60" L"A" L"\u597d");
        CHECK(store->selection().acpStart == 2 &&
              store->selection().acpEnd == 2);

        CHECK(SendKey(key_sink, context, 'D', true));
        CHECK(store->text() == L"\xd83d\xde00" L"A");
        CHECK(store->selection().acpStart == 2 &&
              store->selection().acpEnd == 2);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool PhysicalSelectionKeysAreInterpretedByEngine(
    TextServiceModule *module, const wchar_t *runtime_path) {
  ScopedNeutralLetterKeyboardState keyboard;
  CHECK(keyboard);
  ScopedEnvironment select_keys("FAMO_TEST_SELECT_KEYS", "j0123456789");
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n");
        // 'J' is candidate zero in this schema. TSF forwards the physical key;
        // the engine, not a host-side numeric-index table, commits it.
        CHECK(SendKey(key_sink, context, 'J', true));
        CHECK(store->text() == L"n");

        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"nn");
        // '0' maps beyond the only visible candidate and is therefore
        // unhandled by the schema. It must pass through instead of becoming a
        // fabricated SelectCandidate request.
        CHECK(SendKey(key_sink, context, '0', false));
        CHECK(store->text() == L"nn");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool DigitCanStartCompositionWhenSchemaHandlesIt(
    TextServiceModule *module, const wchar_t *runtime_path) {
  ScopedEnvironment digit_input("FAMO_TEST_DIGIT_INPUT", "1");
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, '7', true));
        CHECK(SendKey(key_sink, context, '7', true));
        CHECK(store->text() == L"7");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool PreviewSelectionRequiresCapabilityAndAuthenticatedRuntimeWindow(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, true, 1));
  const bool passed = RunTextStoreSession(
      module,
      [&](ITfKeyEventSink *key_sink, ITfContext *context,
          FakeTextStore *store, ITfTextInputProcessorEx *service,
          ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");

        HWND target = nullptr;
        famo::runtime::PreviewSelectionRequest request;
        CHECK(module->PreviewSelectionStateForTest(service, &target,
                                                    &request));
        CHECK(target && request.selection_capability &&
              request.composition_sequence != 0);
        // This channel carries clicks from the extra preview page, not the
        // already visible candidate page. With one row/page the first
        // selectable preview candidate has absolute index 1.
        request.absolute_index = 1;

        HWND runtime_source = nullptr;
        const auto source_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!(runtime_source = runtime.PreviewSourceWindow()) &&
               std::chrono::steady_clock::now() < source_deadline) {
          Sleep(1);
        }
        CHECK(runtime_source);
        HWND fake_source = CreateWindowExW(
            0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        CHECK(fake_source);

        const auto send = [&](HWND source,
                              famo::runtime::PreviewSelectionRequest *value) {
          COPYDATASTRUCT copy{
              static_cast<ULONG_PTR>(
                  famo::runtime::kPreviewSelectionCopyDataId),
              static_cast<DWORD>(sizeof(*value)),
              value,
          };
          return SendMessageW(target, WM_COPYDATA,
                              reinterpret_cast<WPARAM>(source),
                              reinterpret_cast<LPARAM>(&copy));
        };

        auto wrong = request;
        wrong.selection_capability.low ^= 1;
        CHECK(send(runtime_source, &wrong) == FALSE);
        auto missing = request;
        missing.selection_capability = {};
        CHECK(send(runtime_source, &missing) == FALSE);
        CHECK(send(nullptr, &request) == FALSE);
        CHECK(send(fake_source, &request) == FALSE);
        CHECK(store->text() == L"ni");

        // An authenticated but out-of-contract current-page index receives an
        // exact rejection. It must not poison/restart the connection.
        auto current_page = request;
        current_page.absolute_index = 0;
        CHECK(send(runtime_source, &current_page) == FALSE);
        CHECK(store->text() == L"ni");
        CHECK(TestKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"nia");
        CHECK(SendKey(key_sink, context, ' ', true));
        CHECK(store->text() == L"nia");
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"niani");
        CHECK(module->PreviewSelectionStateForTest(service, &target,
                                                    &request));
        request.absolute_index = 1;

        CHECK(send(runtime_source, &request) == TRUE);
        // The matching capability is consumed before the delivery boundary.
        CHECK(send(runtime_source, &request) == FALSE);
        const auto apply_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (store->text() == L"niani" &&
               std::chrono::steady_clock::now() < apply_deadline) {
          PumpMessages();
          Sleep(1);
        }
        CHECK(store->text() == L"nia\u5c3c");
        DestroyWindow(fake_source);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

// The UI-less host reaches the candidate list the way TSF documents it: it
// enumerates the begun elements and asks ours for the behavior interface.
bool FindCandidateBehavior(ITfThreadMgr *thread_manager,
                           ITfCandidateListUIElementBehavior **behavior) {
  CHECK(thread_manager && behavior);
  ComPtr<ITfUIElementMgr> ui_manager;
  CHECK(SUCCEEDED(thread_manager->QueryInterface(
      IID_ITfUIElementMgr, reinterpret_cast<void **>(ui_manager.put()))));
  ComPtr<IEnumTfUIElements> elements;
  CHECK(SUCCEEDED(ui_manager->EnumUIElements(elements.put())));
  ITfUIElement *raw = nullptr;
  ULONG fetched = 0;
  while (elements->Next(1, &raw, &fetched) == S_OK && fetched == 1) {
    ComPtr<ITfUIElement> element;
    element.reset(raw);
    raw = nullptr;
    GUID guid{};
    if (FAILED(element->GetGUID(&guid)) || guid != famo::tsf::kCandidateUiGuid)
      continue;
    return SUCCEEDED(element->QueryInterface(
        IID_ITfCandidateListUIElementBehavior,
        reinterpret_cast<void **>(behavior)));
  }
  return false;
}

bool CandidateWindowHonorsHostShowDecision(TextServiceModule *module,
                                           const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  HWND owner = CreateWindowExW(
      0, L"STATIC", L"tsf-candidate-visibility-owner", WS_OVERLAPPEDWINDOW,
      0, 0, 1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  ShowWindow(owner, SW_SHOW);
  CHECK(SetForegroundWindow(owner));

  const bool passed = RunTextStoreSession(
      module,
      [owner](ITfKeyEventSink *key_sink, ITfContext *context, FakeTextStore *,
              ITfTextInputProcessorEx *service, ITfThreadMgr *thread_manager,
              ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        ComPtr<ITfTextLayoutSink> layout_sink;
        CHECK(SUCCEEDED(service->QueryInterface(
            IID_ITfTextLayoutSink,
            reinterpret_cast<void **>(layout_sink.put()))));
        CHECK(SUCCEEDED(
            layout_sink->OnLayoutChange(context, TF_LC_CHANGE, nullptr)));

        CandidateWindowProbe shown;
        CHECK(WaitForProcessCandidateVisibility(true, &shown, owner));
        CHECK(GetWindow(shown.window, GW_OWNER) == owner);
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));

        CHECK(SUCCEEDED(behavior->Show(FALSE)));
        CHECK(WaitForCandidateHidden(shown.window));

        CHECK(SUCCEEDED(behavior->Show(TRUE)));
        CandidateWindowProbe restored;
        CHECK(WaitForProcessCandidateVisibility(true, &restored, owner));
        CHECK(restored.window == shown.window);
        CHECK(GetWindow(restored.window, GW_OWNER) == owner);
        return true;
      },
      nullptr, true, owner);
  CHECK(DestroyWindow(owner));
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool InProcessCandidateClickBindsExactOwner(TextServiceModule *module,
                                            const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  HWND owner = CreateWindowExW(
      0, L"STATIC", L"tsf-candidate-click-owner", WS_OVERLAPPEDWINDOW, 0, 0,
      1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  ShowWindow(owner, SW_SHOW);
  CHECK(SetForegroundWindow(owner));

  const bool passed = RunTextStoreSession(
      module,
      [module, owner](ITfKeyEventSink *key_sink, ITfContext *context,
                      FakeTextStore *store,
                      ITfTextInputProcessorEx *service,
                      ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");
        ComPtr<ITfTextLayoutSink> layout_sink;
        CHECK(SUCCEEDED(service->QueryInterface(
            IID_ITfTextLayoutSink,
            reinterpret_cast<void **>(layout_sink.put()))));
        CHECK(SUCCEEDED(
            layout_sink->OnLayoutChange(context, TF_LC_CHANGE, nullptr)));

        CandidateWindowProbe candidate;
        CHECK(WaitForProcessCandidateVisibility(true, &candidate));
        CHECK(GetWindow(candidate.window, GW_OWNER) == owner);
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));

        HWND target = nullptr;
        famo::runtime::PreviewSelectionRequest request;
        CHECK(module->PreviewSelectionStateForTest(service, &target, &request));
        CHECK(target && request.selection_capability &&
              request.composition_sequence != 0);
        request.absolute_index = 1;
        const auto send = [&](HWND source) {
          COPYDATASTRUCT copy{
              static_cast<ULONG_PTR>(
                  famo::runtime::kPreviewSelectionCopyDataId),
              static_cast<DWORD>(sizeof(request)), &request};
          return SendMessageW(target, WM_COPYDATA,
                              reinterpret_cast<WPARAM>(source),
                              reinterpret_cast<LPARAM>(&copy));
        };

        HWND wrong_source = CreateWindowExW(
            0, L"STATIC", L"wrong-candidate-source", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(wrong_source != nullptr);
        CHECK(send(wrong_source) == FALSE);
        CHECK(send(candidate.window) == TRUE);
        CHECK(send(candidate.window) == FALSE);
        CHECK(DestroyWindow(wrong_source));

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (store->text() == L"ni" &&
               std::chrono::steady_clock::now() < deadline) {
          PumpMessages();
          Sleep(1);
        }
        CHECK(store->text() == L"\u5c3c");
        CandidateWindowProbe hidden;
        CHECK(WaitForProcessCandidateVisibility(false, &hidden));
        ComPtr<ITfCandidateListUIElementBehavior> ended;
        CHECK(!FindCandidateBehavior(thread_manager, ended.put()));
        return true;
      },
      nullptr, true, owner);
  CHECK(DestroyWindow(owner));
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool HostDrivenCandidateBehaviorMatchesRuntime(TextServiceModule *module,
                                               const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");

        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));

        // The element publishes exactly the engine's current page, so the
        // behavior index is page-relative and bounded by that page.
        UINT count = 0;
        CHECK(SUCCEEDED(behavior->GetCount(&count)) && count == 3);
        CHECK(behavior->SetSelection(count) == E_INVALIDARG);
        CHECK(store->text() == L"ni");

        // SetSelection round-trips through the runtime without committing.
        // Only the returned composition changes GetSelection; Finalize then
        // commits the engine's current highlighted candidate.
        CHECK(behavior->SetSelection(1) == S_OK);
        CHECK(store->text() == L"ni");
        UINT selected = 0;
        CHECK(SUCCEEDED(behavior->GetSelection(&selected)) && selected == 1);
        UINT active_count = 0;
        CHECK(SUCCEEDED(behavior->GetCount(&active_count)) &&
              active_count == count);
        CHECK(behavior->Finalize() == S_OK);
        CHECK(store->text() == L"\u5c3c");
        UINT after_commit = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_commit)) &&
              after_commit == 0);
        // The composition is gone. Repeating must fail safely rather than
        // crash the host or poison the runtime connection.
        CHECK(behavior->SetSelection(1) == E_INVALIDARG);
        CHECK(behavior->Finalize() == E_FAIL);
        CHECK(store->text() == L"\u5c3c");

        // Finalize without a preceding SetSelection commits the engine's
        // current choice.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"\u5c3cni");
        CHECK(behavior->Finalize() == S_OK);
        CHECK(store->text() == L"\u5c3c\u4f60");
        CHECK(behavior->Finalize() == E_FAIL);
        CHECK(store->text() == L"\u5c3c\u4f60");

        // Abort drops the composition without committing anything, and stays
        // harmless when there is nothing left to drop.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"\u5c3c\u4f60ni");
        CHECK(behavior->Abort() == S_OK);
        CHECK(store->text() == L"\u5c3c\u4f60");
        CHECK(behavior->Abort() == S_OK);
        CHECK(store->text() == L"\u5c3c\u4f60");

        // Exact finalization commits the displayed preedit literally. It must
        // not auto-convert the engine's highlighted candidate.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        CHECK(store->text() == L"\u5c3c\u4f60nihao");
        ComPtr<ITfIntegratableCandidateListUIElement> integratable;
        CHECK(SUCCEEDED(behavior->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void **>(integratable.put()))));
        CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        CHECK(store->text() == L"\u5c3c\u4f60nihao");
        UINT after_exact = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_exact)) && after_exact == 0);
        CHECK(integratable->FinalizeExactCompositionString() == E_FAIL);

        // The ordinary key path still owns the composition afterwards.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, '2', true));
        CHECK(store->text() == L"\u5c3c\u4f60nihao\u5c3c");

        // Runtime has durably cleared the engine before the host-side exact
        // commit is applied. An allocation failure at that boundary must be
        // retained as deferred delivery, then applied exactly once on retry
        // without wedging the authenticated session.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        CHECK(store->text() == L"\u5c3c\u4f60nihao\u5c3cnihao");
        {
          ScopedEnvironment fail_apply_once(
              "FAMO_TEST_APPLY_COMPOSITION_ALLOCATION_FAILURE_ONCE", "1");
          CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        }
        CHECK(store->text() == L"\u5c3c\u4f60nihao\u5c3cnihao");
        CHECK(TestKey(key_sink, context, 'N', true));
        UINT after_deferred_exact = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_deferred_exact)) &&
              after_deferred_exact == 0);
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"\u5c3c\u4f60nihao\u5c3cnihaon");
        CHECK(behavior->Abort() == S_OK);
        CHECK(store->text() == L"\u5c3c\u4f60nihao\u5c3cnihao");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool NonEmptyExactClearReplyQuarantines(TextServiceModule *module,
                                        const wchar_t *runtime_path) {
  ScopedEnvironment malformed_clear("FAMO_TEST_NONEMPTY_CLEAR_REPLY", "1");
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));
        ComPtr<ITfIntegratableCandidateListUIElement> integratable;
        CHECK(SUCCEEDED(behavior->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void **>(integratable.put()))));

        // The reply is valid wire data but violates ClearComposition's empty
        // result contract. It must be quarantined before its bogus commit can
        // reach the document, and the ended element must reject another call.
        CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        CHECK(store->text() == L"ni");
        CHECK(behavior->SetSelection(1) == E_FAIL);
        CHECK(behavior->Finalize() == E_FAIL);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool ExactFinalizationUsesHostVisibleCandidatePreview(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, true, 0, -1, -1, -1,
                      true));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        // Candidate-preview style replaces raw "nihao" in the host
        // composition with the currently highlighted candidate.
        CHECK(store->text() == L"\u4f60\u597d");
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));
        ComPtr<ITfIntegratableCandidateListUIElement> integratable;
        CHECK(SUCCEEDED(behavior->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void **>(integratable.put()))));

        CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        CHECK(store->text() == L"\u4f60\u597d");
        UINT after_exact = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_exact)) && after_exact == 0);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool ExactFinalizationWithoutInlinePreeditUsesRawComposition(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, false));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        // Disabling inline preedit changes what the candidate window draws,
        // not whether the host holds a composition, so the raw syllables are
        // in the document already. Exact finalization has to keep them
        // verbatim rather than replace them with a converted string.
        CHECK(store->text() == L"nihao");
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));
        ComPtr<ITfIntegratableCandidateListUIElement> integratable;
        CHECK(SUCCEEDED(behavior->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void **>(integratable.put()))));

        CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        CHECK(store->text() == L"nihao");
        UINT after_exact = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_exact)) && after_exact == 0);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool ExactFinalizationBypassesCommitTransforms(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, false, 0, -1, -1, -1,
                      false, true));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(SendKey(key_sink, context, '1', true));
        CHECK(store->text() == L"\u4f60");
        CHECK(SendKey(key_sink, context, 'H', true));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(SendKey(key_sink, context, 'O', true));
        // The committed character plus the live composition, which the host
        // holds whether or not inline preedit is switched on.
        CHECK(store->text() == L"\u4f60hao");
        ComPtr<ITfCandidateListUIElementBehavior> behavior;
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));
        ComPtr<ITfIntegratableCandidateListUIElement> integratable;
        CHECK(SUCCEEDED(behavior->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void **>(integratable.put()))));

        CHECK(integratable->FinalizeExactCompositionString() == S_OK);
        // Ordinary commits add CJK/English spacing under this style. Exact
        // finalization must preserve the displayed raw value byte-for-byte.
        CHECK(store->text() == L"\u4f60hao");
        UINT after_exact = 1;
        CHECK(SUCCEEDED(behavior->GetCount(&after_exact)) && after_exact == 0);
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

// A behavior call after the owning context is gone must fail, not reach into a
// freed session. TSF holds its own reference, so the element outlives it.
bool CandidateBehaviorAfterDeactivationFailsSafely(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  ComPtr<ITfCandidateListUIElementBehavior> behavior;
  const bool passed = RunTextStoreSession(
      module, [&behavior](ITfKeyEventSink *key_sink, ITfContext *context,
                          FakeTextStore *store, ITfTextInputProcessorEx *,
                          ITfThreadMgr *thread_manager, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text() == L"ni");
        CHECK(FindCandidateBehavior(thread_manager, behavior.put()));
        return true;
      });
  CHECK(behavior);
  CHECK(behavior->SetSelection(0) == E_FAIL);
  CHECK(behavior->Finalize() == E_FAIL);
  CHECK(behavior->Abort() == E_FAIL);
  behavior.reset();
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool MissingRuntimeFailsOpen(TextServiceModule *module) {
  std::chrono::steady_clock::duration activation_elapsed{};
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, 'N', false));
        const auto started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'N', false));
        CHECK(std::chrono::steady_clock::now() - started <
              std::chrono::milliseconds(10));
        CHECK(store->text().empty());
        Sleep(550);
        const auto permanent_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'A', false));
        CHECK(std::chrono::steady_clock::now() - permanent_started <
              std::chrono::milliseconds(10));
        return true;
      },
      &activation_elapsed, false);
  CHECK(activation_elapsed < std::chrono::milliseconds(10));
  std::printf("tsf_timing pending_activation_us=%lld\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      activation_elapsed)
                      .count()));
  return passed;
}

bool FaultFailsOpen(TextServiceModule *module, const wchar_t *runtime_path,
                     std::wstring_view fault) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, fault, 1, 2));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n");

        const auto fault_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'I', false));
        CHECK(std::chrono::steady_clock::now() - fault_started <=
              std::chrono::milliseconds(65));
        CHECK(store->text() == L"n");

        const auto pending_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'H', false));
        CHECK(std::chrono::steady_clock::now() - pending_started <
              std::chrono::milliseconds(10));

        const auto recovery_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'A', true) &&
               std::chrono::steady_clock::now() < recovery_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < recovery_deadline);
        std::printf("tsf_timing recovery_ms=%lld\n",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - fault_started)
                            .count()));
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"na");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool StaleFocusedSessionRecoversWithoutFocusChange(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"stale-session", 0, 2));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        // A configuration deploy retires the Runtime session while this host
        // can remain focused indefinitely. The first exact StaleRequest must
        // pass that physical key through, then schedule a fresh session
        // without relying on a later TSF focus notification.
        CHECK(SendKey(key_sink, context, 'N', false));
        CHECK(store->text().empty());

        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'A', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"a");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool RecoveryEditFailureIsCleanedBeforeReconnect(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"engine-hang", 1, 2));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n");
        // What matters is that the denied lock leaves the document untouched,
        // not how many writes seeding the composition took.
        const size_t settled_replaces = store->replace_count();

        store->set_deny_locks(true);
        CHECK(SendKey(key_sink, context, 'I', false));
        store->set_deny_locks(false);
        CHECK(store->text() == L"n" &&
              store->replace_count() == settled_replaces);

        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'A', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"na");
        return true;
      });
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool TerminalDeliveryRecoversWithoutDeactivation(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"copy-failure", 0, 1));
  const auto started = std::chrono::steady_clock::now();
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        // Execute has mutated the engine, but the runtime cannot copy the
        // authoritative result into its wire cache. The key stays eaten and
        // the host must not invent an empty commit or replay the action.
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text().empty());
        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'A', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"a");
        return true;
      });
  CHECK(passed);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::seconds(4));
  // The failed epoch was abandoned and replaced while the TSF activation and
  // test host stayed alive. Deactivation now has no terminal delivery debt.
  CHECK(module->CanUnload());
  CHECK(runtime.Finish());
  return true;
}

bool TerminalSessionPreservesSiblingRecovery(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"copy-failure", 0, 1, true, 0, 1));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(thread_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  // Hold recovery until both sessions in the same authenticated epoch have
  // mutated and are RecoveryPending. The first terminal-result allocation is
  // forced to fail before any destructive abandon; its retry must not replay
  // the business action or send a second terminal command.
  {
    ScopedEnvironment pause_recovery(
        "FAMO_TEST_PAUSE_DELIVERY_RECOVERY", "1");
    ScopedEnvironment fail_terminal_publication(
        "FAMO_TEST_TERMINAL_RESULT_ALLOCATION_FAILURE_ONCE", "1");
    CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(first.document.get(), second.document.get())));
    CHECK(SendKey(key_sink.get(), first.context.get(), 'N', true));
    CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
    CHECK(SUCCEEDED(
        thread_sink->OnSetFocus(second.document.get(), first.document.get())));
    CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));
  }

  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(first.document.get(), second.document.get())));
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), second.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), second.context.get(), ' ', true));
  CHECK(second.store->text() == L"n");

  // Session A alone was tombstoned and reopened. Its lost N action is neither
  // replayed nor allowed to erase B's exact recovered N snapshot.
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(first.document.get(), second.document.get())));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), first.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), first.context.get(), 'A', true));
  CHECK(SendKey(key_sink.get(), first.context.get(), ' ', true));
  CHECK(first.store->text() == L"a");

  thread_sink.reset();
  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool TerminalAbandonLeavesNextActivationIndependent(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess unavailable_after_disconnect;
  CHECK(unavailable_after_disconnect.Start(runtime_path, L"copy-failure", 0,
                                            1));
  CHECK(RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text().empty());
        CHECK(TestKey(key_sink, context, 'A', false));
        return true;
      }));
  CHECK(module->CanUnload());
  CHECK(unavailable_after_disconnect.Finish());

  // In-activation terminal recovery used the authenticated live pipe, so the
  // next activation must not inherit or retry an already-resolved debt.
  RuntimeProcess restarted;
  CHECK(restarted.Start(runtime_path, L"none", 0, 1));
  CHECK(RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, 'N', true));
        CHECK(store->text().empty());
        return true;
      }));
  CHECK(module->CanUnload());
  CHECK(restarted.Finish());
  return true;
}

bool CloseDuringWarmupInvalidatesConnection(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-delay", 0, 2));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(thread_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  Sleep(20);
  CHECK(SUCCEEDED(thread_sink->OnUninitDocumentMgr(first.document.get())));

  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));

  thread_sink.reset();
  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool MultiContextFaultRecoversEveryComposition(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"engine-hang", 2, 2));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));

  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), first.context.get(), 'N', true));
  CHECK(SUCCEEDED(thread_manager->SetFocus(second.document.get())));
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(thread_sink.put()))));
  const auto focus_started = std::chrono::steady_clock::now();
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), first.document.get())));
  const auto focus_elapsed = std::chrono::steady_clock::now() - focus_started;
  CHECK(focus_elapsed < std::chrono::milliseconds(10));
  std::printf("tsf_timing focus_switch_us=%lld\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      focus_elapsed)
                      .count()));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));
  // Seeding a composition may take more than one write. What this has to rule
  // out is one context being serviced and the other dropped, so compare the
  // two rather than pinning a literal count.
  CHECK(first.store->replace_count() > 0 &&
        second.store->replace_count() == first.store->replace_count());

  CHECK(SendKey(key_sink.get(), second.context.get(), 'I', false));
  PumpMessages();
  CHECK(first.store->text() == L"n" && second.store->text() == L"n");
  CHECK(SendKey(key_sink.get(), first.context.get(), 'H', false));
  CHECK(SendKey(key_sink.get(), second.context.get(), 'H', false));

  // Both contexts now own distinct cancellation/recovery work items. The
  // worker must drain both; a single atomic publication slot would overwrite
  // one and leave that context permanently pending.
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!TestKey(key_sink.get(), first.context.get(), 'A', true) ||
          !TestKey(key_sink.get(), second.context.get(), 'A', true)) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), first.context.get(), 'A', true));
  CHECK(SendKey(key_sink.get(), second.context.get(), 'A', true));
  CHECK(first.store->text() == L"na" && second.store->text() == L"na");

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool FocusChurnRejectsObsoleteWarmup(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-delay"));
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  TestDocument second;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &second));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(thread_sink.put()))));
  Sleep(50);

  ITfDocumentMgr *focused = first.document.get();
  for (int index = 0; index < 20; ++index) {
    ITfDocumentMgr *next = index % 2 == 0 ? second.document.get()
                                          : first.document.get();
    const auto started = std::chrono::steady_clock::now();
    CHECK(SUCCEEDED(thread_sink->OnSetFocus(next, focused)));
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::milliseconds(10));
    focused = next;
  }
  const auto final_focus_started = std::chrono::steady_clock::now();
  CHECK(SUCCEEDED(
      thread_sink->OnSetFocus(second.document.get(), focused)));
  CHECK(std::chrono::steady_clock::now() - final_focus_started <
        std::chrono::milliseconds(10));

  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), second.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(TestKey(key_sink.get(), first.context.get(), 'N', false));
  CHECK(SendKey(key_sink.get(), second.context.get(), 'N', true));
  CHECK(first.store->text().empty());
  CHECK(second.store->text() == L"n");

  thread_sink.reset();
  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  CHECK(SUCCEEDED(second.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  second.context.reset();
  first.document.reset();
  second.document.reset();
  first.store.reset();
  second.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool PushPopWarmupCase(TextServiceModule *module, const wchar_t *runtime_path,
                       DWORD before_push_ms) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-delay"));
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));

  TestDocument first;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &first));
  CHECK(SUCCEEDED(thread_manager->SetFocus(first.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  Sleep(before_push_ms);

  ComPtr<FakeTextStore> pushed_store(new FakeTextStore());
  ComPtr<ITfContext> pushed_context;
  TfEditCookie edit_cookie = TF_INVALID_COOKIE;
  CHECK(SUCCEEDED(first.document->CreateContext(
      client_id, 0, static_cast<ITextStoreACP *>(pushed_store.get()),
      pushed_context.put(), &edit_cookie)));
  CHECK(SUCCEEDED(first.document->Push(pushed_context.get())));
  CHECK(SUCCEEDED(first.document->Pop(0)));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), first.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(first.document->Pop(TF_POPF_ALL)));
  first.context.reset();
  pushed_context.reset();
  first.document.reset();
  first.store.reset();
  pushed_store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool PushPopReschedulesSupersededWarmup(
    TextServiceModule *module, const wchar_t *runtime_path) {
  CHECK(PushPopWarmupCase(module, runtime_path, 50));
  CHECK(PushPopWarmupCase(module, runtime_path, 200));
  return true;
}

bool TransientWarmupUnavailableRetriesSameFocus(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-unavailable", 2));
  const bool passed = RunTextStoreSession(
      module,
      [](ITfKeyEventSink *key_sink, ITfContext *context, FakeTextStore *store,
         ITfTextInputProcessorEx *, ITfThreadMgr *, ITfDocumentMgr *) {
        const auto pending_started = std::chrono::steady_clock::now();
        CHECK(TestKey(key_sink, context, 'N', false));
        CHECK(std::chrono::steady_clock::now() - pending_started <
              std::chrono::milliseconds(10));

        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'N', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n");
        return true;
      },
      nullptr, false);
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool TimedOutWarmupReconnectsSameFocus(TextServiceModule *module,
                                       const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-hang", 0, 2));
  const bool passed = RunTextStoreSession(
      module,
      [](ITfKeyEventSink *key_sink, ITfContext *context, FakeTextStore *store,
         ITfTextInputProcessorEx *, ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, 'N', false));
        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!TestKey(key_sink, context, 'N', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n");
        return true;
      },
      nullptr, false);
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool ExhaustedWarmupRetriesOnNextKeySameFocus(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-unavailable", 3));
  const bool passed = RunTextStoreSession(
      module,
      [](ITfKeyEventSink *key_sink, ITfContext *context, FakeTextStore *store,
         ITfTextInputProcessorEx *, ITfThreadMgr *, ITfDocumentMgr *) {
        Sleep(100);
        const auto retry_started = std::chrono::steady_clock::now();
        CHECK(SendKey(key_sink, context, 'N', false));
        CHECK(std::chrono::steady_clock::now() - retry_started <
              std::chrono::milliseconds(10));
        CHECK(store->text().empty());

        const auto ready_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!TestKey(key_sink, context, 'A', true) &&
               std::chrono::steady_clock::now() < ready_deadline) {
          PumpMessages();
          Sleep(5);
        }
        CHECK(std::chrono::steady_clock::now() < ready_deadline);
        CHECK(SendKey(key_sink, context, 'A', true));
        CHECK(store->text() == L"a");
        return true;
      },
      nullptr, false);
  const bool runtime_finished = runtime.Finish();
  return passed && runtime_finished;
}

bool ReactivationRejectsOldGeneration(TextServiceModule *module,
                                      const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-hang", 0, 2));
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  // Deactivate while the delayed OpenSession is still in flight. Shutdown must
  // cancel that control-path request instead of joining through its deadline.
  Sleep(50);
  const auto deactivate_started = std::chrono::steady_clock::now();
  CHECK(SUCCEEDED(service->Deactivate()));
  std::printf("tsf_timing in_flight_deactivate_us=%lld\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - deactivate_started)
                      .count()));
  CHECK(std::chrono::steady_clock::now() - deactivate_started <
         std::chrono::milliseconds(50));
  key_sink.reset();

  const auto reactivate_started = std::chrono::steady_clock::now();
  const HRESULT reactivated = module->ReactivateForTest(
      service.get(), thread_manager.get(), client_id);
  std::printf("tsf_timing reactivate_us=%lld\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - reactivate_started)
                      .count()));
  CHECK(SUCCEEDED(reactivated));
  CHECK(std::chrono::steady_clock::now() - reactivate_started <
         std::chrono::milliseconds(10));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(TestKey(key_sink.get(), target.context.get(), 'N', false));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
  CHECK(target.store->text() == L"n");

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool ForegroundFocusRecyclesRuntimeConnection(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 3));
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> first;
  ComPtr<ITfTextInputProcessorEx> second;
  ComPtr<ITfKeyEventSink> first_keys;
  ComPtr<ITfKeyEventSink> second_keys;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        first.put())));
  CHECK(SUCCEEDED(first->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(first_keys.put()))));
  auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(first_keys.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(first_keys->OnSetFocus(FALSE)));

  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        second.put())));
  CHECK(SUCCEEDED(second->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(second_keys.put()))));
  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(second_keys.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SUCCEEDED(second_keys->OnSetFocus(FALSE)));
  CHECK(SUCCEEDED(first_keys->OnSetFocus(TRUE)));

  ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(first_keys.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(first_keys.get(), target.context.get(), 'N', true));
  CHECK(target.store->text() == L"n");

  second_keys.reset();
  CHECK(SUCCEEDED(second->Deactivate()));
  second.reset();
  first_keys.reset();
  CHECK(SUCCEEDED(first->Deactivate()));
  first.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  CHECK(runtime.Finish());
  return true;
}

bool SetKeyboardDisabled(ITfContext *context, TfClientId client_id,
                         bool value) {
  ComPtr<ITfCompartmentMgr> compartments;
  CHECK(SUCCEEDED(context->QueryInterface(
      IID_ITfCompartmentMgr, reinterpret_cast<void **>(compartments.put()))));
  ComPtr<ITfCompartment> disabled;
  CHECK(SUCCEEDED(compartments->GetCompartment(
      GUID_COMPARTMENT_KEYBOARD_DISABLED, disabled.put())));
  VARIANT setting;
  VariantInit(&setting);
  setting.vt = VT_I4;
  setting.lVal = value ? 1 : 0;
  const HRESULT result = disabled->SetValue(client_id, &setting);
  VariantClear(&setting);
  return SUCCEEDED(result);
}

bool HasActiveComposition(ITfContext *context, bool *active) {
  CHECK(context && active);
  *active = false;
  ComPtr<ITfContextOwnerCompositionServices> services;
  CHECK(SUCCEEDED(context->QueryInterface(
      IID_ITfContextOwnerCompositionServices,
      reinterpret_cast<void **>(services.put()))));
  ComPtr<IEnumITfCompositionView> compositions;
  CHECK(SUCCEEDED(services->EnumCompositions(compositions.put())));
  ITfCompositionView *raw = nullptr;
  ULONG fetched = 0;
  const HRESULT next = compositions->Next(1, &raw, &fetched);
  if (raw)
    raw->Release();
  CHECK(next == S_OK || next == S_FALSE);
  *active = next == S_OK && fetched == 1;
  return true;
}

bool DisabledKeyboardContextPassesKeysThrough(TextServiceModule *module,
                                              const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  HWND owner = CreateWindowExW(
      0, L"STATIC", L"tsf-password-security-owner", WS_OVERLAPPEDWINDOW, 0,
      0, 1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  target.store->set_window_for_test(owner);
  ShowWindow(owner, SW_SHOW);
  CHECK(SetForegroundWindow(owner));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  ComPtr<ITfThreadMgrEventSink> thread_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(thread_sink.put()))));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  // A disabled context must fail open before a Runtime session is scheduled.
  // Waiting proves the disabled context never starts eating keys later.
  TestDocument password;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &password));
  CHECK(SetKeyboardDisabled(password.context.get(), client_id, true));
  CHECK(SUCCEEDED(thread_manager->SetFocus(password.document.get())));
  CHECK(SUCCEEDED(thread_sink->OnSetFocus(password.document.get(),
                                          target.document.get())));
  const auto disabled_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < disabled_deadline) {
    CHECK(TestKey(key_sink.get(), password.context.get(), 'N', false));
    PumpMessages();
    Sleep(5);
  }
  CHECK(SendKey(key_sink.get(), password.context.get(), 'N', false));
  CHECK(password.store->text().empty());

  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));
  CHECK(SUCCEEDED(thread_sink->OnSetFocus(target.document.get(),
                                          password.document.get())));
  CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
  CHECK(SendKey(key_sink.get(), target.context.get(), 'I', true));
  CHECK(target.store->text() == L"ni");
  bool composing = false;
  CHECK(HasActiveComposition(target.context.get(), &composing) && composing);
  ComPtr<ITfCandidateListUIElementBehavior> behavior;
  CHECK(FindCandidateBehavior(thread_manager.get(), behavior.put()));
  HWND original_preview_target = nullptr;
  famo::runtime::PreviewSelectionRequest original_preview;
  CHECK(module->PreviewSelectionStateForTest(
      service.get(), &original_preview_target, &original_preview));
  CandidateWindowProbe security_candidate;
  CHECK(WaitForProcessCandidateVisibility(true, &security_candidate, owner));
  CHECK(GetWindow(security_candidate.window, GW_OWNER) == owner);

  // Hosts can disable a context while it is focused. The TIP must end its
  // existing composition and candidate element before passing later keys on.
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
  CHECK(WaitForCandidateHidden(security_candidate.window));
  CHECK(HasActiveComposition(target.context.get(), &composing) && !composing);
  ComPtr<ITfCandidateListUIElementBehavior> hidden_behavior;
  CHECK(!FindCandidateBehavior(thread_manager.get(), hidden_behavior.put()));
  CHECK(FAILED(behavior->SetSelection(0)));

  CHECK(TestKey(key_sink.get(), target.context.get(), 'N', false));
  CHECK(SendKey(key_sink.get(), target.context.get(), 'N', false));
  CHECK(target.store->text() == L"ni");

  behavior.reset();

  // Re-enabling the same focused context must use a fresh Runtime session,
  // never the state that was retired at the security boundary.
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto reenabled_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'H', true) &&
         std::chrono::steady_clock::now() < reenabled_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < reenabled_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'H', true));
  CHECK(target.store->text().find(L'h') != std::wstring::npos);
  const std::wstring reenabled_text = target.store->text();
  CHECK(HasActiveComposition(target.context.get(), &composing) && composing);
  ComPtr<ITfCandidateListUIElementBehavior> retry_behavior;
  CHECK(FindCandidateBehavior(thread_manager.get(), retry_behavior.put()));
  HWND fresh_preview_target = nullptr;
  famo::runtime::PreviewSelectionRequest fresh_preview;
  CHECK(module->PreviewSelectionStateForTest(
      service.get(), &fresh_preview_target, &fresh_preview));
  CHECK(fresh_preview.correlation.session_id !=
            original_preview.correlation.session_id ||
        fresh_preview.correlation.session_generation !=
            original_preview.correlation.session_generation);

  // A denied synchronous edit lock leaves cleanup debt. The candidate element
  // still ends immediately, and the recovery window retries composition End
  // after the host becomes writable without waiting for another user key.
  target.store->set_deny_locks(true);
  const size_t lock_requests_before_security_close =
      target.store->lock_request_count();
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
  CHECK(HasActiveComposition(target.context.get(), &composing) && composing);
  ComPtr<ITfCandidateListUIElementBehavior> denied_behavior;
  CHECK(!FindCandidateBehavior(thread_manager.get(), denied_behavior.put()));
  CHECK(FAILED(retry_behavior->SetSelection(0)));
  // Exhaust the original bounded retry budget while the host keeps denying
  // locks. This guards both against an accidental permanent 100 Hz poll and
  // against a later compartment transition inheriting a spent budget.
  const auto bounded_retry_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < bounded_retry_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(HasActiveComposition(target.context.get(), &composing) && composing);
  CHECK(target.store->lock_request_count() -
            lock_requests_before_security_close <=
        28);

  // Re-enable while cleanup is still denied. OnChange must reset the bounded
  // retry opportunity; after the host unlocks, message pumping alone must end
  // the stale composition before any new key is used to await a fresh session.
  const size_t lock_requests_before_reenable =
      target.store->lock_request_count();
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto reenable_retry_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
  while (std::chrono::steady_clock::now() < reenable_retry_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(target.store->lock_request_count() >
        lock_requests_before_reenable);
  CHECK(HasActiveComposition(target.context.get(), &composing) && composing);
  target.store->set_deny_locks(false);
  const auto cleanup_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (HasActiveComposition(target.context.get(), &composing) && composing &&
         std::chrono::steady_clock::now() < cleanup_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < cleanup_deadline && !composing);
  CHECK(target.store->text() == reenabled_text);
  const auto fresh_session_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < fresh_session_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < fresh_session_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'A', true));
  CHECK(target.store->text().find(L'a') != std::wstring::npos);

  retry_behavior.reset();
  thread_sink.reset();
  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(password.document->Pop(TF_POPF_ALL)));
  password.context.reset();
  password.document.reset();
  password.store.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.store->set_window_for_test(nullptr);
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(DestroyWindow(owner));
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return runtime.Finish();
}

bool DisabledDuringWarmupRejectsLateSession(TextServiceModule *module,
                                            const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"open-session-delay"));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));

  Sleep(20);
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
  const auto stale_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < stale_deadline) {
    PumpMessages();
    CHECK(TestKey(key_sink.get(), target.context.get(), 'N', false));
    Sleep(5);
  }
  bool composing = false;
  CHECK(HasActiveComposition(target.context.get(), &composing) && !composing);
  ComPtr<ITfCandidateListUIElementBehavior> behavior;
  CHECK(!FindCandidateBehavior(thread_manager.get(), behavior.put()));

  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto fresh_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < fresh_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < fresh_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'A', true));
  CHECK(target.store->text() == L"a");

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return runtime.Finish();
}

bool DisabledAfterPreparedClaimSkipsRecoveryExecute(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"disconnect-before-execute", 0, 3,
                      true, 0, 1));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));

  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  const uint32_t claims_before = module->RecoveryPreparedClaimsForTest();
  const uint32_t executes_before = module->RecoveryExecuteAttemptsForTest();
  {
    // The first transport disconnects after Prepare. Hold the worker only
    // after its authenticated Claim confirms Prepared, then cross the real
    // compartment boundary on the activation thread.
    ScopedEnvironment pause_after_claim(
        "FAMO_TEST_PAUSE_AFTER_RECOVERY_CLAIM", "1");
    CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
    CHECK(target.store->text().empty());
    const auto claim_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (module->RecoveryPreparedClaimsForTest() == claims_before &&
           std::chrono::steady_clock::now() < claim_deadline) {
      PumpMessages();
      Sleep(5);
    }
    CHECK(std::chrono::steady_clock::now() < claim_deadline);
    CHECK(module->RecoveryExecuteAttemptsForTest() == executes_before);
    CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
    CHECK(TestKey(key_sink.get(), target.context.get(), 'N', false));
  }

  // Releasing the paused worker must observe the security gate, skip
  // ExecutePrepared, retire the connection, and open a genuinely fresh
  // session when the same context is enabled again.
  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto fresh_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), target.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < fresh_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < fresh_deadline);
  CHECK(module->RecoveryExecuteAttemptsForTest() == executes_before);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'A', true));
  CHECK(target.store->text().find(L'a') != std::wstring::npos);
  CHECK(target.store->text().find(L'n') == std::wstring::npos);

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return runtime.Finish();
}

bool TerminalRetirePreservesSecurityAbandon(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"copy-failure", 0, 2, true, 0, 2));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);

  const uint32_t publication_ready_before =
      module->TerminalPublicationReadyForTest();
  const uint32_t retired_before = module->TerminalRetiredSessionsForTest();
  {
    // Keep the worker parked after publishing too, so the activation thread
    // deterministically runs RetireAbandonedSession while a different epoch
    // occupies the modeled priority slot and this connection-wide security
    // Abandon remains in the ordinary bounded queue.
    ScopedEnvironment pause_after_publication(
        "FAMO_TEST_PAUSE_AFTER_TERMINAL_PUBLICATION", "1");
    {
      ScopedEnvironment pause_before_publication(
          "FAMO_TEST_PAUSE_BEFORE_TERMINAL_PUBLICATION", "1");
      ScopedEnvironment force_different_priority(
          "FAMO_TEST_FORCE_DIFFERENT_EPOCH_SECURITY_PRIORITY", "1");
      CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
      CHECK(target.store->text().empty());
      const auto publication_ready_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (module->TerminalPublicationReadyForTest() ==
                 publication_ready_before &&
             std::chrono::steady_clock::now() < publication_ready_deadline) {
        PumpMessages();
        Sleep(5);
      }
      CHECK(std::chrono::steady_clock::now() <
            publication_ready_deadline);
      CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
      PumpMessages();
      CHECK(TestKey(key_sink.get(), target.context.get(), 'N', false));
    }

    const auto retire_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (module->TerminalRetiredSessionsForTest() == retired_before &&
           std::chrono::steady_clock::now() < retire_deadline) {
      PumpMessages();
      Sleep(5);
    }
    CHECK(std::chrono::steady_clock::now() < retire_deadline);
    CHECK(module->TerminalRetiredSessionsForTest() > retired_before);
    CHECK(target.store->text().empty());
  }

  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto fresh_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!TestKey(key_sink.get(), target.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < fresh_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < fresh_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'A', true));
  CHECK(target.store->text().find(L'a') != std::wstring::npos);
  CHECK(target.store->text().find(L'n') == std::wstring::npos);

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return runtime.Finish();
}

bool DisabledPendingDeliveryIsAbandoned(TextServiceModule *module,
                                        const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 2, true, 0, 1));

  ComPtr<ITfThreadMgr> thread_manager;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
      reinterpret_cast<void **>(thread_manager.put()))));
  TfClientId client_id = TF_CLIENTID_NULL;
  CHECK(SUCCEEDED(thread_manager->Activate(&client_id)));
  TestDocument target;
  CHECK(CreateTestDocument(thread_manager.get(), client_id, &target));
  CHECK(SUCCEEDED(thread_manager->SetFocus(target.document.get())));

  ComPtr<ITfTextInputProcessorEx> service;
  CHECK(SUCCEEDED(module->CreateForTest(thread_manager.get(), client_id,
                                        service.put())));
  ComPtr<ITfKeyEventSink> key_sink;
  CHECK(SUCCEEDED(service->QueryInterface(
      IID_ITfKeyEventSink, reinterpret_cast<void **>(key_sink.put()))));

  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'N', true) &&
         std::chrono::steady_clock::now() < ready_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < ready_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'N', true));
  CHECK(target.store->text() == L"n");

  // The Runtime has durably prepared this synthetic key, but the host cannot
  // apply its composition. Disabling the context must retire the authenticated
  // connection that owns that delivery instead of applying it later when edit
  // locks become available.
  target.store->set_deny_locks(true);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'I', true));
  CHECK(target.store->text() == L"n");
  {
    ScopedEnvironment fail_first_security_abandon(
        "FAMO_TEST_SECURITY_ABANDON_ALLOCATION_FAILURE_ONCE", "1");
    CHECK(SetKeyboardDisabled(target.context.get(), client_id, true));
    target.store->set_deny_locks(false);
    PumpMessages();
    CHECK(GetEnvironmentVariableA(
              "FAMO_TEST_SECURITY_ABANDON_ALLOCATION_FAILURE_ONCE", nullptr,
              0) == 0);
  }

  bool composing = true;
  const auto disabled_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < disabled_deadline) {
    PumpMessages();
    CHECK(TestKey(key_sink.get(), target.context.get(), 'A', false));
    CHECK(target.store->text() == L"n");
    CHECK(HasActiveComposition(target.context.get(), &composing));
    if (!composing)
      break;
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < disabled_deadline && !composing);
  ComPtr<ITfCandidateListUIElementBehavior> behavior;
  CHECK(!FindCandidateBehavior(thread_manager.get(), behavior.put()));

  CHECK(SetKeyboardDisabled(target.context.get(), client_id, false));
  const auto fresh_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!TestKey(key_sink.get(), target.context.get(), 'A', true) &&
         std::chrono::steady_clock::now() < fresh_deadline) {
    PumpMessages();
    Sleep(5);
  }
  CHECK(std::chrono::steady_clock::now() < fresh_deadline);
  CHECK(SendKey(key_sink.get(), target.context.get(), 'A', true));
  CHECK(target.store->text().find(L'a') != std::wstring::npos);
  CHECK(target.store->text().find(L'i') == std::wstring::npos);

  key_sink.reset();
  CHECK(SUCCEEDED(service->Deactivate()));
  service.reset();
  CHECK(SUCCEEDED(target.document->Pop(TF_POPF_ALL)));
  target.context.reset();
  target.document.reset();
  target.store.reset();
  CHECK(SUCCEEDED(thread_manager->Deactivate()));
  thread_manager.reset();
  return runtime.Finish();
}

bool AllTextStoreChecks(const wchar_t *module_path,
                        const wchar_t *runtime_path) {
  ScopedCom com;
  CHECK(SUCCEEDED(com.result()));
  TextServiceModule module;
  CHECK(module.Load(module_path));
  CHECK(AllocationBoundariesReleaseReferences(&module));
  CHECK(ActivationPublishesOpenInputMode(&module));
  CHECK(ForcedDeactivationAbandonsTerminalDelivery(&module, runtime_path));
  CHECK(TerminalPublicationSlotSerializesFailures(&module, runtime_path));
  CHECK(MissingRuntimeFailsOpen(&module));
  CHECK(HealthyRoundtrip(&module, runtime_path));
  CHECK(OptionalContextSinkFailureStillTypes(
      &module, runtime_path,
      "FAMO_TEST_KEYBOARD_DISABLED_SINK_UNAVAILABLE"));
  CHECK(OptionalContextSinkFailureStillTypes(
      &module, runtime_path, "FAMO_TEST_LAYOUT_SINK_UNAVAILABLE"));
  CHECK(CompositionLayoutUsesActiveRangeEnd(&module, runtime_path));
  CHECK(ReentrantActivationFocusStillBeginsCandidates(&module, runtime_path));
  CHECK(CandidateWindowUsesContextViewOwner(&module, runtime_path));
  CHECK(CandidateWindowHonorsHostShowDecision(&module, runtime_path));
  CHECK(InProcessCandidateClickBindsExactOwner(&module, runtime_path));
  CHECK(SearchCandidateProviderIsDiscoverable(&module, runtime_path));
  CHECK(DisabledKeyboardContextPassesKeysThrough(&module, runtime_path));
  CHECK(DisabledDuringWarmupRejectsLateSession(&module, runtime_path));
  CHECK(DisabledAfterPreparedClaimSkipsRecoveryExecute(&module,
                                                       runtime_path));
  CHECK(TerminalRetirePreservesSecurityAbandon(&module, runtime_path));
  CHECK(DisabledPendingDeliveryIsAbandoned(&module, runtime_path));
  CHECK(DisabledInlinePreeditStillComposesInHost(&module, runtime_path));
  CHECK(InlinePreeditPreservesUtf16Selection(&module, runtime_path));
  CHECK(PhysicalSelectionKeysAreInterpretedByEngine(&module, runtime_path));
  CHECK(DigitCanStartCompositionWhenSchemaHandlesIt(&module, runtime_path));
  CHECK(PreviewSelectionRequiresCapabilityAndAuthenticatedRuntimeWindow(
      &module, runtime_path));
  CHECK(HostDrivenCandidateBehaviorMatchesRuntime(&module, runtime_path));
  CHECK(NonEmptyExactClearReplyQuarantines(&module, runtime_path));
  CHECK(ExactFinalizationUsesHostVisibleCandidatePreview(&module,
                                                         runtime_path));
  CHECK(ExactFinalizationWithoutInlinePreeditUsesRawComposition(
      &module, runtime_path));
  CHECK(ExactFinalizationBypassesCommitTransforms(&module, runtime_path));
  CHECK(CandidateBehaviorAfterDeactivationFailsSafely(&module, runtime_path));
  CHECK(FaultFailsOpen(&module, runtime_path, L"engine-hang"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"disconnect"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"malformed"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"late"));
  CHECK(StaleFocusedSessionRecoversWithoutFocusChange(&module,
                                                       runtime_path));
  CHECK(RecoveryEditFailureIsCleanedBeforeReconnect(&module, runtime_path));
  CHECK(TerminalDeliveryRecoversWithoutDeactivation(&module, runtime_path));
  CHECK(TerminalSessionPreservesSiblingRecovery(&module, runtime_path));
  CHECK(TerminalAbandonLeavesNextActivationIndependent(&module,
                                                       runtime_path));
  CHECK(CloseDuringWarmupInvalidatesConnection(&module, runtime_path));
  CHECK(MultiContextFaultRecoversEveryComposition(&module, runtime_path));
  CHECK(FocusChurnRejectsObsoleteWarmup(&module, runtime_path));
  CHECK(PushPopReschedulesSupersededWarmup(&module, runtime_path));
  CHECK(TransientWarmupUnavailableRetriesSameFocus(&module, runtime_path));
  CHECK(TimedOutWarmupReconnectsSameFocus(&module, runtime_path));
  CHECK(ExhaustedWarmupRetriesOnNextKeySameFocus(&module, runtime_path));
  CHECK(ReactivationRejectsOldGeneration(&module, runtime_path));
  CHECK(ForegroundFocusRecyclesRuntimeConnection(&module, runtime_path));
  CHECK(ForcedDeactivationAbandonsIdleContexts(&module, runtime_path));
  CHECK(ForcedDeactivationAttemptsUnavailableEpochOnce(&module,
                                                       runtime_path));
  CHECK(module.CanUnload());
  return true;
}

bool OptionalContextSinkChecks(const wchar_t *module_path,
                               const wchar_t *runtime_path) {
  ScopedCom com;
  CHECK(SUCCEEDED(com.result()));
  TextServiceModule module;
  CHECK(module.Load(module_path));
  CHECK(OptionalContextSinkFailureStillTypes(
      &module, runtime_path,
      "FAMO_TEST_KEYBOARD_DISABLED_SINK_UNAVAILABLE"));
  CHECK(OptionalContextSinkFailureStillTypes(
      &module, runtime_path, "FAMO_TEST_LAYOUT_SINK_UNAVAILABLE"));
  CHECK(module.CanUnload());
  return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc == 4 && std::wstring_view(argv[3]) == L"optional-context-sinks") {
    if (!OptionalContextSinkChecks(argv[1], argv[2]))
      return 1;
    std::printf("tsf_optional_context_sinks: OK\n");
    return 0;
  }
  if (argc == 4 && std::wstring_view(argv[3]) == L"reentrant-activation") {
    ScopedCom com;
    if (FAILED(com.result()))
      return 1;
    TextServiceModule module;
    if (!module.Load(argv[1]) ||
        !ReentrantActivationFocusStillBeginsCandidates(&module, argv[2]))
      return 1;
    std::printf("tsf_reentrant_activation: OK\n");
    return 0;
  }
  if (argc != 3)
    return 2;
  if (!AllTextStoreChecks(argv[1], argv[2]))
    return 1;
  std::printf("tsf_integration_selfcheck: OK\n");
  return 0;
}
