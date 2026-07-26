#include <chrono>
#include <cstdio>
#include <functional>

#include <msctf.h>
#include <windows.h>

#include "com_ptr.h"
#include "fake_text_store.h"
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

bool RunTextStoreSession(
    TextServiceModule *module, const SessionCheck &check,
    std::chrono::steady_clock::duration *activation_elapsed = nullptr,
    bool wait_for_session = true) {
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

bool HealthyRoundtrip(TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(TestKey(key_sink, context, 'N', true));
        CHECK(TestKey(key_sink, context, 'N', true));
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

bool DisabledInlinePreeditStaysOutOfHost(TextServiceModule *module,
                                         const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"none", 0, 1, false));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(SendKey(key_sink, context, 'I', true));
        CHECK(store->text().empty());
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

        CHECK(SendKey(key_sink, context, 'B', true));
        CHECK(store->text() == L"abcdef");
        CHECK(store->selection().acpStart == 1 &&
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

bool RecoveryEditFailureIsCleanedBeforeReconnect(
    TextServiceModule *module, const wchar_t *runtime_path) {
  RuntimeProcess runtime;
  CHECK(runtime.Start(runtime_path, L"engine-hang", 1, 2));
  const bool passed = RunTextStoreSession(
      module, [](ITfKeyEventSink *key_sink, ITfContext *context,
                 FakeTextStore *store, ITfTextInputProcessorEx *,
                 ITfThreadMgr *, ITfDocumentMgr *) {
        CHECK(SendKey(key_sink, context, 'N', true));
        CHECK(store->text() == L"n" && store->replace_count() == 1);

        store->set_deny_locks(true);
        CHECK(SendKey(key_sink, context, 'I', false));
        store->set_deny_locks(false);
        CHECK(store->text() == L"n" && store->replace_count() == 1);

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
  CHECK(runtime.Start(runtime_path, L"engine-hang", 2));

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
  CHECK(first.store->replace_count() == 1 &&
        second.store->replace_count() == 1);

  CHECK(SendKey(key_sink.get(), second.context.get(), 'I', false));
  PumpMessages();
  CHECK(first.store->text() == L"n" && second.store->text() == L"n");
  CHECK(SendKey(key_sink.get(), first.context.get(), 'H', false));
  CHECK(SendKey(key_sink.get(), second.context.get(), 'H', false));

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

bool AllTextStoreChecks(const wchar_t *module_path,
                        const wchar_t *runtime_path) {
  ScopedCom com;
  CHECK(SUCCEEDED(com.result()));
  TextServiceModule module;
  CHECK(module.Load(module_path));
  CHECK(MissingRuntimeFailsOpen(&module));
  CHECK(HealthyRoundtrip(&module, runtime_path));
  CHECK(DisabledInlinePreeditStaysOutOfHost(&module, runtime_path));
  CHECK(InlinePreeditPreservesUtf16Selection(&module, runtime_path));
  CHECK(FaultFailsOpen(&module, runtime_path, L"engine-hang"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"disconnect"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"malformed"));
  CHECK(FaultFailsOpen(&module, runtime_path, L"late"));
  CHECK(RecoveryEditFailureIsCleanedBeforeReconnect(&module, runtime_path));
  CHECK(CloseDuringWarmupInvalidatesConnection(&module, runtime_path));
  CHECK(MultiContextFaultRecoversEveryComposition(&module, runtime_path));
  CHECK(FocusChurnRejectsObsoleteWarmup(&module, runtime_path));
  CHECK(PushPopReschedulesSupersededWarmup(&module, runtime_path));
  CHECK(TransientWarmupUnavailableRetriesSameFocus(&module, runtime_path));
  CHECK(TimedOutWarmupReconnectsSameFocus(&module, runtime_path));
  CHECK(ExhaustedWarmupRetriesOnNextKeySameFocus(&module, runtime_path));
  CHECK(ReactivationRejectsOldGeneration(&module, runtime_path));
  CHECK(ForegroundFocusRecyclesRuntimeConnection(&module, runtime_path));
  CHECK(module.CanUnload());
  return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc != 3)
    return 2;
  if (!AllTextStoreChecks(argv[1], argv[2]))
    return 1;
  std::printf("tsf_integration_selfcheck: OK\n");
  return 0;
}
