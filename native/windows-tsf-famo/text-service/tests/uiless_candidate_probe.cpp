// Repeatable UI-less A/B candidate probe (issue #37).
//
//   uiless_candidate_probe --mode allow|deny|show-false
//
// Hosts a real TSF thread manager, advises its own ITfUIElementSink, loads the
// shipping FamoTextService.dll in process and drives one composition against the
// shipping FamoRuntime.exe on a private endpoint. The candidate window, its
// visibility gate and the whole wire path are the production ones. The probe
// installs nothing, registers nothing, and terminates only the runtime process
// it started itself. One JSON object is written to stdout; exit code 0 means
// every expectation of the mode held.
//
// Control direction. The text service calls ITfUIElementMgr::BeginUIElement;
// TSF then calls this probe's ITfUIElementSink::BeginUIElement, and the value
// the probe writes into pbShow is what decides whether the input method may draw
// its own candidate UI. That single BOOL is the whole A/B. The probe never
// forces it TRUE and never reaches around the negotiation to show the window.
//
// Staging. Two things in the shipping binaries are pinned and cannot be
// redirected from outside without changing product code:
//   * the pipe peers verify each other by full image path (VerifyProcess in
//     pipe_security.cpp), and FamoCreateTextServiceForTest pins the server to
//     "<FamoTextService.dll directory>\FamoTestRuntime.exe";
//   * that same export pins the schema id to "test".
// So the probe assembles a private directory: the production FamoRuntime under
// the pinned server name, and FamoTestEngine under the name FamoRuntime loads
// its engine from. The runtime, the candidate window and the protocol are
// production code; only the engine behind them is the deterministic test engine,
// which is what makes repeated runs comparable. The engine is not a party to the
// pbShow negotiation this probe judges. Real-application acceptance is T7.
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <msctf.h>
#include <oleauto.h>
#include <windows.h>

#include "com_ptr.h"
#include "fake_text_store.h"
#include "famo_runtime_pipe.h"
#include "tsf_integration_support.h"

namespace {

using famo::tsf::ComPtr;
using famo::tsf::test::FakeTextStore;
using famo::tsf::test::TextServiceModule;

constexpr wchar_t kCandidateWindowClass[] = L"FamoRuntimeCandidateWindow";
// The expected commit: 你好.
constexpr wchar_t kExpectedCommit[] = L"你好";
constexpr auto kReadyTimeout = std::chrono::seconds(30);
// Every window verdict is taken over a fixed window rather than a single
// sample: a "hidden" claim is only worth anything if the runtime had time to
// draw and still did not.
constexpr auto kWatchWindow = std::chrono::milliseconds(2000);

enum class Mode { Allow, Deny, ShowFalse };

struct Failure {
  std::string check;
  std::string detail;
};

struct ElementEvent {
  const char *event;
  DWORD id;
  // Only meaningful for begin: what TSF proposed and what the probe answered.
  int show_in = -1;
  int show_out = -1;
};

struct Watch {
  std::string stage;
  bool present = false;
  bool ever_visible = false;
  bool final_visible = false;
  RECT rect{};
};

struct CandidateList {
  bool available = false;
  UINT count = 0;
  UINT selection = 0;
  std::wstring selected;
  int is_shown = -1;
};

std::vector<Failure> g_failures;

void Fail(std::string check, std::string detail) {
  g_failures.push_back({std::move(check), std::move(detail)});
}

void Expect(bool condition, std::string check, std::string detail) {
  if (!condition)
    Fail(std::move(check), std::move(detail));
}

std::wstring ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::string JsonString(std::wstring_view value) {
  std::string quoted = "\"";
  char escape[8]{};
  for (const wchar_t unit : value) {
    if (unit == L'"' || unit == L'\\') {
      quoted.push_back('\\');
      quoted.push_back(static_cast<char>(unit));
    } else if (unit >= 0x20 && unit < 0x7f) {
      quoted.push_back(static_cast<char>(unit));
    } else {
      std::snprintf(escape, sizeof(escape), "\\u%04x",
                    static_cast<unsigned>(unit));
      quoted += escape;
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::string JsonString(std::string_view value) {
  return JsonString(std::wstring(value.begin(), value.end()));
}

const char *JsonBool(bool value) { return value ? "true" : "false"; }

// -1 records "not observed" rather than silently reporting a false.
const char *JsonTriState(int value) {
  return value < 0 ? "null" : (value ? "true" : "false");
}

std::string CodePoints(std::wstring_view value) {
  std::string list = "[";
  for (size_t index = 0; index < value.size(); ++index) {
    uint32_t point = value[index];
    if (point >= 0xd800 && point <= 0xdbff && index + 1 < value.size() &&
        value[index + 1] >= 0xdc00 && value[index + 1] <= 0xdfff) {
      point = 0x10000 + ((point - 0xd800) << 10) + (value[++index] - 0xdc00);
    }
    if (list.size() > 1)
      list += ", ";
    list += std::to_string(point);
  }
  return list + "]";
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

// The desktop may already host a production runtime, so a class-name match
// alone is not enough: only the window owned by the process this probe started
// is evidence about this run.
HWND FindCandidateWindow(DWORD process_id) {
  HWND after = nullptr;
  while ((after = FindWindowExW(nullptr, after, kCandidateWindowClass,
                                nullptr))) {
    DWORD owner = 0;
    if (GetWindowThreadProcessId(after, &owner) != 0 && owner == process_id)
      return after;
  }
  return nullptr;
}

// Samples for the whole watch window and reports both whether the window was
// ever visible and where it ended up. "Never showed" and "showed then hid" are
// different verdicts, and the caller decides which one its mode requires.
Watch WatchWindow(std::string stage, DWORD process_id) {
  const auto deadline = std::chrono::steady_clock::now() + kWatchWindow;
  Watch watch;
  watch.stage = std::move(stage);
  for (;;) {
    const HWND window = FindCandidateWindow(process_id);
    watch.present = window != nullptr;
    watch.final_visible = window && IsWindowVisible(window);
    watch.ever_visible = watch.ever_visible || watch.final_visible;
    if (window)
      GetWindowRect(window, &watch.rect);
    if (std::chrono::steady_clock::now() >= deadline)
      return watch;
    PumpMessages();
    Sleep(10);
  }
}

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

// The host half of the UI-less negotiation. Advised on the thread manager
// through ITfSource, exactly as a UI-less application would advise it.
class UiElementSink final : public ITfUIElementSink {
public:
  explicit UiElementSink(bool allow_show) : allow_show_(allow_show) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfUIElementSink)
      *object = static_cast<ITfUIElementSink *>(this);
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

  HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id, BOOL *show) override {
    if (!show)
      return E_POINTER;
    const BOOL answer = allow_show_ ? TRUE : FALSE;
    events_.push_back({"begin", id, *show ? 1 : 0, answer ? 1 : 0});
    *show = answer;
    element_id_ = id;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
    events_.push_back({"update", id});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
    events_.push_back({"end", id});
    return S_OK;
  }

  const std::vector<ElementEvent> &events() const { return events_; }
  DWORD element_id() const { return element_id_; }

private:
  ~UiElementSink() = default;

  std::atomic<ULONG> references_{1};
  bool allow_show_;
  DWORD element_id_ = TF_INVALID_UIELEMENTID;
  std::vector<ElementEvent> events_;
};

// A private copy of the shipping binaries, so the probe never writes into the
// build tree and the pinned peer image path is one the probe fully owns.
class StagedBinaries {
public:
  ~StagedBinaries() { Remove(); }

  bool Create() {
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0)
      return false;
    directory_ = std::wstring(temp) + L"famo-uiless-probe-" +
                 std::to_wstring(GetCurrentProcessId());
    Remove();
    if (!CreateDirectoryW(directory_.c_str(), nullptr))
      return false;
    created_ = true;
    return CreateDirectoryW(data_root().c_str(), nullptr) &&
           Stage(L"FamoRuntime.exe", L"FamoTestRuntime.exe") &&
           Stage(L"FamoTestEngine.dll", L"FamoRimeEngine.dll") &&
           Stage(L"FamoTextService.dll", L"FamoTextService.dll");
  }

  std::wstring runtime() const { return directory_ + L"\\FamoTestRuntime.exe"; }
  std::wstring text_service() const {
    return directory_ + L"\\FamoTextService.dll";
  }
  std::wstring data_root() const { return directory_ + L"\\data"; }
  std::wstring source(const wchar_t *name) const {
    return ModuleDirectory() + L"\\" + name;
  }

  // Called once the module is unloaded and the runtime has exited, so the
  // probe leaves nothing behind even across repeated runs.
  void Remove() {
    if (directory_.empty())
      return;
    for (const std::wstring &name : staged_)
      DeleteFileW((directory_ + L"\\" + name).c_str());
    staged_.clear();
    RemoveDataRoot();
    if (created_ && RemoveDirectoryW(directory_.c_str()))
      created_ = false;
  }

private:
  // The runtime only ever writes flat control state here, so one non-recursive
  // sweep is enough and cannot reach outside the staged directory.
  void RemoveDataRoot() {
    const std::wstring pattern = data_root() + L"\\*";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
          DeleteFileW((data_root() + L"\\" + found.cFileName).c_str());
      } while (FindNextFileW(search, &found));
      FindClose(search);
    }
    RemoveDirectoryW(data_root().c_str());
  }

  bool Stage(const wchar_t *from, const wchar_t *to) {
    if (!CopyFileW(source(from).c_str(), (directory_ + L"\\" + to).c_str(),
                   FALSE)) {
      return false;
    }
    staged_.emplace_back(to);
    return true;
  }

  std::wstring directory_;
  std::vector<std::wstring> staged_;
  bool created_ = false;
};

// The production runtime, started on a private endpoint so it never collides
// with an installed one and never takes the default-suffix install-state path.
class ProbeRuntime {
public:
  ~ProbeRuntime() { Stop(); }

  bool Start(const std::wstring &executable, const std::wstring &suffix,
             const std::wstring &data_root) {
    std::wstring command = L"\"" + executable + L"\" --endpoint-suffix " +
                           suffix + L" --data-root \"" + data_root + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, ModuleDirectory().c_str(),
                        &startup, &process_)) {
      return false;
    }
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;

    famo::runtime::PipeEndpoint endpoint;
    std::string error;
    if (!famo::runtime::BuildCurrentPipeEndpoint(suffix, &endpoint, &error))
      return false;
    const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (WaitNamedPipeW(endpoint.name.c_str(), 50) &&
          FindCandidateWindow(process_.dwProcessId)) {
        return true;
      }
      if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0)
        return false;
      Sleep(20);
    }
    return false;
  }

  // The runtime's own control endpoint cannot be used here: its control server
  // pins clients to its own image path while the control client pins the server
  // to "FamoRuntime.exe", and staging cannot satisfy both. Terminating a process
  // this probe started, after the composition is complete, is deterministic and
  // risks no user data — the engine is the test engine and the data root is the
  // probe's own throwaway directory.
  bool Stop() {
    if (!process_.hProcess)
      return false;
    bool exited = WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0;
    if (!exited) {
      TerminateProcess(process_.hProcess, 0);
      exited = WaitForSingleObject(process_.hProcess, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle(process_.hProcess);
    process_.hProcess = nullptr;
    return exited;
  }

  DWORD process_id() const { return process_.dwProcessId; }

private:
  PROCESS_INFORMATION process_{};
};

bool TestKey(ITfKeyEventSink *sink, ITfContext *context, WPARAM key) {
  BOOL eaten = FALSE;
  return SUCCEEDED(sink->OnTestKeyDown(context, key, 0, &eaten)) && eaten;
}

bool SendKey(ITfKeyEventSink *sink, ITfContext *context, WPARAM key) {
  BOOL eaten = FALSE;
  return SUCCEEDED(sink->OnKeyDown(context, key, 0, &eaten)) && eaten;
}

// The UI-less payload a host would render itself. Reading it back through the
// element manager is what proves the data is still available when the probe has
// denied the input method its own window.
CandidateList ReadCandidateList(ITfUIElementMgr *manager, DWORD id) {
  CandidateList list;
  if (!manager || id == TF_INVALID_UIELEMENTID)
    return list;
  ComPtr<ITfUIElement> element;
  if (FAILED(manager->GetUIElement(id, element.put())) || !element)
    return list;
  ComPtr<ITfCandidateListUIElement> candidates;
  if (FAILED(element->QueryInterface(
          IID_ITfCandidateListUIElement,
          reinterpret_cast<void **>(candidates.put())))) {
    return list;
  }
  list.available = true;
  candidates->GetCount(&list.count);
  candidates->GetSelection(&list.selection);
  BOOL shown = FALSE;
  if (SUCCEEDED(element->IsShown(&shown)))
    list.is_shown = shown ? 1 : 0;
  BSTR value = nullptr;
  if (list.count > 0 && SUCCEEDED(candidates->GetString(list.selection,
                                                        &value)) &&
      value) {
    list.selected.assign(value, SysStringLen(value));
    SysFreeString(value);
  }
  return list;
}

bool ShowElement(ITfUIElementMgr *manager, DWORD id, BOOL show) {
  ComPtr<ITfUIElement> element;
  if (!manager || id == TF_INVALID_UIELEMENTID ||
      FAILED(manager->GetUIElement(id, element.put())) || !element) {
    return false;
  }
  return SUCCEEDED(element->Show(show));
}

struct ProbeResult {
  std::vector<Watch> watches;
  std::vector<ElementEvent> events;
  CandidateList candidates;
  std::wstring commit;
  std::wstring text_service_source;
  std::wstring runtime_source;
  bool runtime_started = false;
  bool service_activated = false;
  bool sink_advised = false;
  bool session_ready = false;
  bool show_false_sent = false;
  bool clean_shutdown = false;
};

void Drive(Mode mode, ITfKeyEventSink *keys, ITfContext *context,
           FakeTextStore *store, ITfUIElementMgr *manager,
           UiElementSink *sink, DWORD runtime_pid, ProbeResult *result) {
  result->watches.push_back(WatchWindow("before_keys", runtime_pid));

  for (const WPARAM key : {'N', 'I', 'H', 'A', 'O'}) {
    Expect(SendKey(keys, context, key), "key_eaten",
           "the runtime did not handle a composition key");
    PumpMessages();
  }

  result->watches.push_back(WatchWindow("candidates", runtime_pid));
  result->candidates = ReadCandidateList(manager, sink->element_id());

  if (mode == Mode::ShowFalse) {
    // Issue #38's criterion: a host that starts drawing for itself mid-session
    // calls Show(FALSE) and the input method's own window must go away.
    result->show_false_sent = ShowElement(manager, sink->element_id(), FALSE);
    Expect(result->show_false_sent, "show_false_sent",
           "ITfUIElement::Show(FALSE) could not be delivered");
    PumpMessages();
    result->watches.push_back(WatchWindow("after_show_false", runtime_pid));
  }

  Expect(SendKey(keys, context, VK_SPACE), "commit_eaten",
         "the commit key was not handled");
  PumpMessages();
  result->commit = store->text();
  result->watches.push_back(WatchWindow("after_commit", runtime_pid));
}

bool Run(Mode mode, ProbeResult *result) {
  // The in-process activation export dials this endpoint, so the probe's own
  // runtime has to serve exactly it.
  const std::wstring suffix = famo::tsf::test::TestEndpointSuffix();

  StagedBinaries staged;
  if (!staged.Create()) {
    Fail("stage", "could not stage the runtime and text service privately");
    return false;
  }
  result->text_service_source = staged.source(L"FamoTextService.dll");
  result->runtime_source = staged.source(L"FamoRuntime.exe");

  ProbeRuntime runtime;
  if (!runtime.Start(staged.runtime(), suffix, staged.data_root())) {
    Fail("runtime_start",
         "FamoRuntime did not reach a serving pipe and a candidate window");
    return false;
  }
  result->runtime_started = true;

  bool host_ready = false;
  {
    TextServiceModule module;
    if (!module.Load(staged.text_service().c_str())) {
      Fail("module_load", "FamoTextService.dll exports missing");
      result->clean_shutdown = runtime.Stop();
      return false;
    }

    ComPtr<ITfThreadMgr> thread_manager;
    TfClientId client_id = TF_CLIENTID_NULL;
    ComPtr<ITfDocumentMgr> document;
    ComPtr<FakeTextStore> store;
    ComPtr<ITfContext> context;
    ComPtr<ITfUIElementMgr> ui_manager;
    ComPtr<ITfSource> source;
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    TfEditCookie edit_cookie = TF_INVALID_COOKIE;
    DWORD sink_cookie = TF_INVALID_COOKIE;
    UiElementSink *sink = new UiElementSink(mode != Mode::Deny);

    host_ready =
        SUCCEEDED(CoCreateInstance(
            CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
            reinterpret_cast<void **>(thread_manager.put()))) &&
        SUCCEEDED(thread_manager->Activate(&client_id)) &&
        SUCCEEDED(thread_manager->QueryInterface(
            IID_ITfUIElementMgr,
            reinterpret_cast<void **>(ui_manager.put()))) &&
        SUCCEEDED(thread_manager->QueryInterface(
            IID_ITfSource, reinterpret_cast<void **>(source.put()))) &&
        SUCCEEDED(thread_manager->CreateDocumentMgr(document.put()));
    if (host_ready) {
      // Advised before the text service is activated, so the very first
      // BeginUIElement of the session is the probe's to answer.
      result->sink_advised = SUCCEEDED(source->AdviseSink(
          IID_ITfUIElementSink, static_cast<ITfUIElementSink *>(sink),
          &sink_cookie));
      store.reset(new FakeTextStore());
      host_ready =
          result->sink_advised &&
          SUCCEEDED(document->CreateContext(
              client_id, 0, static_cast<ITextStoreACP *>(store.get()),
              context.put(), &edit_cookie)) &&
          SUCCEEDED(document->Push(context.get())) &&
          SUCCEEDED(thread_manager->SetFocus(document.get()));
    }
    Expect(host_ready, "tsf_host",
           "thread manager, UI element sink or document setup failed");

    if (host_ready) {
      if (FAILED(module.CreateForTest(thread_manager.get(), client_id,
                                      service.put())) ||
          FAILED(service->QueryInterface(
              IID_ITfKeyEventSink, reinterpret_cast<void **>(keys.put())))) {
        Fail("activate", "FamoTextService activation failed");
      } else {
        result->service_activated = true;
        const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
        while (!TestKey(keys.get(), context.get(), 'N') &&
               std::chrono::steady_clock::now() < deadline) {
          PumpMessages();
          Sleep(10);
        }
        result->session_ready = TestKey(keys.get(), context.get(), 'N');
        Expect(result->session_ready, "session_ready",
               "runtime session never accepted a key");
        if (result->session_ready) {
          Drive(mode, keys.get(), context.get(), store.get(), ui_manager.get(),
                sink, runtime.process_id(), result);
        }
      }
    }

    result->events = sink->events();
    keys.reset();
    bool torn_down = true;
    if (service)
      torn_down = SUCCEEDED(service->Deactivate());
    service.reset();
    if (result->sink_advised)
      torn_down = SUCCEEDED(source->UnadviseSink(sink_cookie)) && torn_down;
    sink->Release();
    if (document)
      torn_down = SUCCEEDED(document->Pop(TF_POPF_ALL)) && torn_down;
    context.reset();
    document.reset();
    store.reset();
    source.reset();
    ui_manager.reset();
    if (thread_manager)
      torn_down = SUCCEEDED(thread_manager->Deactivate()) && torn_down;
    thread_manager.reset();
    torn_down = module.CanUnload() && torn_down;
    result->clean_shutdown = torn_down;
  }
  result->clean_shutdown = runtime.Stop() && result->clean_shutdown;
  Expect(result->clean_shutdown, "clean_shutdown",
         "the probe could not tear its host and runtime down deterministically");
  return host_ready;
}

void Check(Mode mode, const ProbeResult &result) {
  if (!result.session_ready)
    return;

  const auto watch = [&result](std::string_view stage) -> const Watch * {
    for (const Watch &entry : result.watches) {
      if (entry.stage == stage)
        return &entry;
    }
    return nullptr;
  };

  Expect(result.commit == kExpectedCommit, "commit_text",
         "the composition did not commit U+4F60 U+597D");

  size_t begins = 0;
  for (const ElementEvent &event : result.events) {
    if (std::string_view(event.event) == "begin")
      ++begins;
  }
  Expect(begins > 0, "element_begun",
         "the input method never opened a UI element through the sink");
  Expect(result.candidates.available && result.candidates.count > 0,
         "uiless_data",
         "the candidate list was not readable through ITfUIElementMgr");

  const Watch *candidates = watch("candidates");
  if (!candidates) {
    Fail("candidates_watch", "the candidate stage was never observed");
    return;
  }
  if (mode == Mode::Deny) {
    Expect(!candidates->ever_visible, "window_denied",
           "the self-drawn candidate window appeared although the host answered "
           "pbShow=FALSE");
  } else {
    Expect(candidates->ever_visible, "window_shown",
           "the self-drawn candidate window never appeared although the host "
           "answered pbShow=TRUE");
  }

  if (mode == Mode::ShowFalse) {
    const Watch *hidden = watch("after_show_false");
    Expect(hidden && !hidden->final_visible, "window_hidden_on_show_false",
           "ITfUIElement::Show(FALSE) did not hide the self-drawn candidate "
           "window");
  }

  const Watch *committed = watch("after_commit");
  Expect(committed && !committed->final_visible, "window_hidden_after_commit",
         "the candidate window stayed visible after the composition committed");
}

void Report(Mode mode, const ProbeResult &result) {
  const char *mode_name = mode == Mode::Allow  ? "allow"
                          : mode == Mode::Deny ? "deny"
                                               : "show-false";
  std::printf("{\n");
  std::printf("  \"probe\": \"uiless_candidate_probe\",\n");
  std::printf("  \"issue\": 37,\n");
  std::printf("  \"mode\": \"%s\",\n", mode_name);
  std::printf("  \"text_service_module\": %s,\n",
              JsonString(result.text_service_source).c_str());
  std::printf("  \"runtime_image\": %s,\n",
              JsonString(result.runtime_source).c_str());
  std::printf("  \"sink_advised\": %s,\n", JsonBool(result.sink_advised));
  std::printf("  \"runtime_started\": %s,\n", JsonBool(result.runtime_started));
  std::printf("  \"service_activated\": %s,\n",
              JsonBool(result.service_activated));
  std::printf("  \"session_ready\": %s,\n", JsonBool(result.session_ready));
  std::printf("  \"show_false_sent\": %s,\n", JsonBool(result.show_false_sent));

  std::printf("  \"element_events\": [\n");
  for (size_t index = 0; index < result.events.size(); ++index) {
    const ElementEvent &event = result.events[index];
    std::printf("    {\"event\": \"%s\", \"id\": %lu, \"show_in\": %s, "
                "\"show_out\": %s}%s\n",
                event.event, static_cast<unsigned long>(event.id),
                JsonTriState(event.show_in), JsonTriState(event.show_out),
                index + 1 == result.events.size() ? "" : ",");
  }
  std::printf("  ],\n");

  std::printf("  \"candidate_list\": {\"available\": %s, \"count\": %u, "
              "\"selection\": %u, \"selected\": %s, \"is_shown\": %s},\n",
              JsonBool(result.candidates.available), result.candidates.count,
              result.candidates.selection,
              JsonString(result.candidates.selected).c_str(),
              JsonTriState(result.candidates.is_shown));

  std::printf("  \"window\": [\n");
  for (size_t index = 0; index < result.watches.size(); ++index) {
    const Watch &entry = result.watches[index];
    std::printf("    {\"stage\": %s, \"present\": %s, \"ever_visible\": %s, "
                "\"final_visible\": %s, \"rect\": [%ld, %ld, %ld, %ld]}%s\n",
                JsonString(entry.stage).c_str(), JsonBool(entry.present),
                JsonBool(entry.ever_visible), JsonBool(entry.final_visible),
                entry.rect.left, entry.rect.top, entry.rect.right,
                entry.rect.bottom,
                index + 1 == result.watches.size() ? "" : ",");
  }
  std::printf("  ],\n");

  std::printf("  \"commit_utf16\": %s,\n", JsonString(result.commit).c_str());
  std::printf("  \"commit_code_points\": %s,\n",
              CodePoints(result.commit).c_str());
  std::printf("  \"clean_shutdown\": %s,\n", JsonBool(result.clean_shutdown));
  std::printf("  \"failures\": [\n");
  for (size_t index = 0; index < g_failures.size(); ++index) {
    std::printf("    {\"check\": %s, \"detail\": %s}%s\n",
                JsonString(g_failures[index].check).c_str(),
                JsonString(g_failures[index].detail).c_str(),
                index + 1 == g_failures.size() ? "" : ",");
  }
  std::printf("  ],\n");
  std::printf("  \"passed\": %s\n", JsonBool(g_failures.empty()));
  std::printf("}\n");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  Mode mode = Mode::Allow;
  bool mode_set = false;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--mode" && index + 1 < argc) {
      const std::wstring_view value(argv[++index]);
      mode_set = true;
      if (value == L"allow")
        mode = Mode::Allow;
      else if (value == L"deny")
        mode = Mode::Deny;
      else if (value == L"show-false")
        mode = Mode::ShowFalse;
      else
        mode_set = false;
    } else {
      mode_set = false;
      break;
    }
  }
  if (!mode_set) {
    std::fprintf(stderr,
                 "usage: uiless_candidate_probe --mode allow|deny|show-false\n");
    return 2;
  }

  ScopedCom com;
  if (FAILED(com.result())) {
    std::fprintf(stderr, "COM apartment setup failed\n");
    return 2;
  }
  ProbeResult result;
  Run(mode, &result);
  Check(mode, result);
  Report(mode, result);
  return g_failures.empty() ? 0 : 1;
}
