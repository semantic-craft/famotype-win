// Repeatable UI-less A/B candidate probe (issue #37).
//
//   uiless_candidate_probe --mode <mode>
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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ctffunc.h>
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

enum class Mode {
  Allow,
  Deny,
  ShowFalse,
  BehaviorSelect,
  BehaviorFinalize,
  BehaviorAbort,
  IntegratableKeys,
  BehaviorInvalid,
};

const char *ModeName(Mode mode) {
  switch (mode) {
  case Mode::Allow:
    return "allow";
  case Mode::Deny:
    return "deny";
  case Mode::ShowFalse:
    return "show-false";
  case Mode::BehaviorSelect:
    return "behavior-select";
  case Mode::BehaviorFinalize:
    return "behavior-finalize";
  case Mode::BehaviorAbort:
    return "behavior-abort";
  case Mode::IntegratableKeys:
    return "integratable-keys";
  case Mode::BehaviorInvalid:
    return "behavior-invalid";
  }
  return "unknown";
}

bool HostAllowsSelfDraw(Mode mode) {
  return mode == Mode::Allow || mode == Mode::ShowFalse;
}

bool ExercisesCandidateControl(Mode mode) {
  return mode == Mode::BehaviorSelect || mode == Mode::BehaviorFinalize ||
         mode == Mode::BehaviorAbort || mode == Mode::IntegratableKeys ||
         mode == Mode::BehaviorInvalid;
}

int PrimaryIssue(Mode mode) {
  if (mode == Mode::IntegratableKeys)
    return 40;
  return ExercisesCandidateControl(mode) ? 39 : 37;
}

const char *CoveredIssues(Mode mode) {
  if (mode == Mode::BehaviorInvalid)
    return "[39, 40]";
  if (mode == Mode::IntegratableKeys)
    return "[40]";
  return ExercisesCandidateControl(mode) ? "[39]" : "[37]";
}

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
  bool count_called = false;
  HRESULT count_result = E_NOTIMPL;
  bool selection_called = false;
  HRESULT selection_result = E_NOTIMPL;
  UINT count = 0;
  UINT selection = 0;
  std::wstring selected;
  int is_shown = -1;
};

struct BehaviorObservation {
  bool available = false;
  bool set_selection_called = false;
  HRESULT set_selection_result = E_NOTIMPL;
  bool finalize_called = false;
  HRESULT finalize_result = E_NOTIMPL;
  bool abort_called = false;
  HRESULT abort_result = E_NOTIMPL;
};

struct IntegratableObservation {
  bool available = false;
  bool distinct_interface_addresses = false;
  bool selection_style_called = false;
  HRESULT selection_style_result = E_NOTIMPL;
  TfIntegratableCandidateListSelectionStyle selection_style =
      STYLE_IMPLIED_SELECTION;
  bool show_numbers_called = false;
  HRESULT show_numbers_result = E_NOTIMPL;
  int show_numbers = -1;
  bool key_called = false;
  HRESULT key_result = E_NOTIMPL;
  int key_eaten = -1;
  bool finalize_exact_called = false;
  HRESULT finalize_exact_result = E_NOTIMPL;
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

std::string JsonHresult(bool called, HRESULT result) {
  return called ? std::to_string(static_cast<long>(result)) : "null";
}

std::string JsonNumber(bool observed, long value) {
  return observed ? std::to_string(value) : "null";
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

// Covers the gaps between the fixed-duration stage watches: key callbacks and
// Behavior/Integratable COM calls can synchronously change Runtime UI state.
// This worker never touches COM; it continuously samples only the candidate
// window owned by the exact Runtime process this probe started.
class ContinuousVisibilityWatch {
public:
  explicit ContinuousVisibilityWatch(DWORD process_id)
      : process_id_(process_id) {}
  ~ContinuousVisibilityWatch() { Stop(); }

  bool Start() {
    if (process_id_ == 0 || worker_.joinable())
      return false;
    stop_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    try {
      worker_ = std::thread([this] { Run(); });
    } catch (...) {
      return false;
    }
    while (!ready_.load(std::memory_order_acquire))
      SwitchToThread();
    return true;
  }

  Watch Finish(std::string stage) {
    Stop();
    Watch result;
    result.stage = std::move(stage);
    result.present = present_;
    result.ever_visible = ever_visible_;
    result.final_visible = final_visible_;
    result.rect = rect_;
    return result;
  }

private:
  void Sample() noexcept {
    const HWND window = FindCandidateWindow(process_id_);
    present_ = window != nullptr;
    final_visible_ = window && IsWindowVisible(window);
    ever_visible_ = ever_visible_ || final_visible_;
    if (window)
      GetWindowRect(window, &rect_);
  }

  void Run() noexcept {
    Sample();
    ready_.store(true, std::memory_order_release);
    while (!stop_.load(std::memory_order_acquire)) {
      Sleep(1);
      Sample();
    }
    Sample();
  }

  void Stop() noexcept {
    if (!worker_.joinable())
      return;
    stop_.store(true, std::memory_order_release);
    worker_.join();
  }

  DWORD process_id_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<bool> ready_{false};
  std::thread worker_;
  bool present_ = false;
  bool ever_visible_ = false;
  bool final_visible_ = false;
  RECT rect_{};
};

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
           CreateDirectoryW(lock_root().c_str(), nullptr) &&
           Stage(L"FamoRuntime.exe", L"FamoTestRuntime.exe") &&
           Stage(L"FamoTestEngine.dll", L"FamoRimeEngine.dll") &&
           Stage(L"FamoTextService.dll", L"FamoTextService.dll");
  }

  std::wstring runtime() const { return directory_ + L"\\FamoTestRuntime.exe"; }
  std::wstring text_service() const {
    return directory_ + L"\\FamoTextService.dll";
  }
  std::wstring data_root() const { return directory_ + L"\\data"; }
  std::wstring lock_root() const { return directory_ + L"\\locks"; }
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
    RemoveLockRoot();
    if (created_ && RemoveDirectoryW(directory_.c_str()))
      created_ = false;
  }

private:
  // The runtime only ever writes flat control state here, so one non-recursive
  // sweep is enough and cannot reach outside the staged directory.
  void RemoveDataRoot() {
    const std::wstring log_directory = data_root() + L"\\log";
    DeleteFileW((log_directory + L"\\famo-runtime-startup.log").c_str());
    RemoveDirectoryW(log_directory.c_str());
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

  void RemoveLockRoot() {
    const std::wstring lock_directory = lock_root() + L"\\Famo.UserDataLocks";
    const std::wstring pattern = lock_directory + L"\\*";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
          DeleteFileW((lock_directory + L"\\" + found.cFileName).c_str());
      } while (FindNextFileW(search, &found));
      FindClose(search);
    }
    RemoveDirectoryW(lock_directory.c_str());
    RemoveDirectoryW(lock_root().c_str());
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
             const std::wstring &data_root, const std::wstring &lock_root) {
    std::wstring command = L"\"" + executable + L"\" --endpoint-suffix " +
                           suffix + L" --data-root \"" + data_root + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    // This runtime owns a throwaway data root. Keep its test-only user-data
    // lock private as well so an installed Runtime (or an MSIX-hosted runner's
    // LOCALAPPDATA virtualization) cannot affect the probe verdict.
    constexpr wchar_t kLockRootEnvironment[] =
        L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA";
    const DWORD previous_size =
        GetEnvironmentVariableW(kLockRootEnvironment, nullptr, 0);
    std::wstring previous;
    if (previous_size > 0) {
      previous.assign(previous_size, L'\0');
      const DWORD copied = GetEnvironmentVariableW(
          kLockRootEnvironment, previous.data(), previous_size);
      if (copied == 0 || copied >= previous_size)
        return false;
      previous.resize(copied);
    }
    if (!SetEnvironmentVariableW(kLockRootEnvironment, lock_root.c_str()))
      return false;
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, ModuleDirectory().c_str(), &startup, &process_);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    const BOOL restored = SetEnvironmentVariableW(
        kLockRootEnvironment, previous_size > 0 ? previous.c_str() : nullptr);
    const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    if (!created || !restored) {
      if (created) {
        TerminateProcess(process_.hProcess, 1);
        WaitForSingleObject(process_.hProcess, 5000);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
        process_ = {};
      }
      SetLastError(created ? restore_error : create_error);
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
CandidateList ReadCandidateList(ITfCandidateListUIElement *candidates);

CandidateList ReadCandidateList(ITfUIElementMgr *manager, DWORD id) {
  if (!manager || id == TF_INVALID_UIELEMENTID)
    return {};
  ComPtr<ITfUIElement> element;
  if (FAILED(manager->GetUIElement(id, element.put())) || !element)
    return {};
  ComPtr<ITfCandidateListUIElement> candidates;
  if (FAILED(element->QueryInterface(
          IID_ITfCandidateListUIElement,
          reinterpret_cast<void **>(candidates.put())))) {
    return {};
  }
  return ReadCandidateList(candidates.get());
}

struct CandidateInterfaces {
  ComPtr<ITfCandidateListUIElementBehavior> behavior;
  ComPtr<ITfIntegratableCandidateListUIElement> integratable;
};

CandidateInterfaces ReadCandidateInterfaces(ITfUIElementMgr *manager,
                                            DWORD id) {
  CandidateInterfaces interfaces;
  if (!manager || id == TF_INVALID_UIELEMENTID)
    return interfaces;
  ComPtr<ITfUIElement> element;
  if (FAILED(manager->GetUIElement(id, element.put())) || !element)
    return interfaces;
  element->QueryInterface(IID_ITfCandidateListUIElementBehavior,
                          reinterpret_cast<void **>(interfaces.behavior.put()));
  element->QueryInterface(
      IID_ITfIntegratableCandidateListUIElement,
      reinterpret_cast<void **>(interfaces.integratable.put()));
  return interfaces;
}

CandidateList ReadCandidateList(ITfCandidateListUIElement *candidates) {
  CandidateList list;
  if (!candidates)
    return list;
  list.count_called = true;
  list.count_result = candidates->GetCount(&list.count);
  if (FAILED(list.count_result))
    return list;
  list.selection_called = true;
  list.selection_result = candidates->GetSelection(&list.selection);
  if (FAILED(list.selection_result) ||
      (list.count == 0 ? list.selection != 0
                       : list.selection >= list.count)) {
    if (SUCCEEDED(list.selection_result))
      list.selection_result = E_UNEXPECTED;
    return list;
  }
  list.available = true;
  BOOL shown = FALSE;
  if (SUCCEEDED(candidates->IsShown(&shown)))
    list.is_shown = shown ? 1 : 0;
  BSTR value = nullptr;
  if (list.count > 0 && list.selection < list.count &&
      SUCCEEDED(candidates->GetString(list.selection, &value)) && value) {
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
  CandidateList candidates_after_action;
  CandidateList candidates_final;
  BehaviorObservation behavior;
  IntegratableObservation integratable;
  std::wstring document_after_action;
  std::wstring commit;
  std::wstring text_service_source;
  std::wstring runtime_source;
  bool runtime_started = false;
  bool service_activated = false;
  bool sink_advised = false;
  bool session_ready = false;
  bool show_false_sent = false;
  bool continuous_visibility_started = false;
  bool clean_shutdown = false;
};

void Drive(Mode mode, ITfKeyEventSink *keys, ITfContext *context,
           FakeTextStore *store, ITfUIElementMgr *manager,
           UiElementSink *sink, DWORD runtime_pid, ProbeResult *result) {
  ContinuousVisibilityWatch continuous_visibility(runtime_pid);
  result->continuous_visibility_started = continuous_visibility.Start();
  Expect(result->continuous_visibility_started, "continuous_window_watch",
         "the full-drive candidate-window monitor could not start");
  const auto finish_visibility = [&] {
    if (result->continuous_visibility_started) {
      result->watches.push_back(
          continuous_visibility.Finish("full_drive"));
    }
  };
  result->watches.push_back(WatchWindow("before_keys", runtime_pid));

  for (const WPARAM key : {'N', 'I', 'H', 'A', 'O'}) {
    Expect(SendKey(keys, context, key), "key_eaten",
           "the runtime did not handle a composition key");
    PumpMessages();
  }

  result->watches.push_back(WatchWindow("candidates", runtime_pid));
  result->candidates = ReadCandidateList(manager, sink->element_id());

  if (ExercisesCandidateControl(mode)) {
    CandidateInterfaces interfaces =
        ReadCandidateInterfaces(manager, sink->element_id());
    result->behavior.available = static_cast<bool>(interfaces.behavior);
    result->integratable.available =
        static_cast<bool>(interfaces.integratable);
    result->integratable.distinct_interface_addresses =
        interfaces.behavior && interfaces.integratable &&
        static_cast<void *>(interfaces.behavior.get()) !=
            static_cast<void *>(interfaces.integratable.get());

    if (mode == Mode::BehaviorSelect && interfaces.behavior) {
      result->behavior.set_selection_called = true;
      result->behavior.set_selection_result =
          interfaces.behavior->SetSelection(1);
      PumpMessages();
      result->candidates_after_action =
          ReadCandidateList(interfaces.behavior.get());
      result->candidates_final = result->candidates_after_action;
      result->document_after_action = store->text();
      result->watches.push_back(
          WatchWindow("after_behavior_select", runtime_pid));
      finish_visibility();
      return;
    }

    if (mode == Mode::BehaviorFinalize && interfaces.behavior) {
      result->behavior.set_selection_called = true;
      result->behavior.set_selection_result =
          interfaces.behavior->SetSelection(0);
      PumpMessages();
      result->candidates_after_action =
          ReadCandidateList(interfaces.behavior.get());
      result->behavior.finalize_called = true;
      result->behavior.finalize_result = interfaces.behavior->Finalize();
      PumpMessages();
      result->commit = store->text();
      result->document_after_action = result->commit;
      result->candidates_final = ReadCandidateList(interfaces.behavior.get());
      result->watches.push_back(
          WatchWindow("after_behavior_finalize", runtime_pid));
      finish_visibility();
      return;
    }

    if (mode == Mode::BehaviorAbort && interfaces.behavior) {
      result->behavior.abort_called = true;
      result->behavior.abort_result = interfaces.behavior->Abort();
      PumpMessages();
      result->document_after_action = store->text();
      result->candidates_after_action =
          ReadCandidateList(interfaces.behavior.get());
      result->candidates_final = result->candidates_after_action;
      result->watches.push_back(
          WatchWindow("after_behavior_abort", runtime_pid));
      finish_visibility();
      return;
    }

    if (mode == Mode::IntegratableKeys && interfaces.integratable) {
      result->integratable.selection_style_called = true;
      result->integratable.selection_style_result =
          interfaces.integratable->GetSelectionStyle(
              &result->integratable.selection_style);
      BOOL show_numbers = FALSE;
      result->integratable.show_numbers_called = true;
      result->integratable.show_numbers_result =
          interfaces.integratable->ShowCandidateNumbers(&show_numbers);
      result->integratable.show_numbers = show_numbers ? 1 : 0;
      BOOL eaten = FALSE;
      result->integratable.key_called = true;
      result->integratable.key_result =
          interfaces.integratable->OnKeyDown('1', 0, &eaten);
      result->integratable.key_eaten = eaten ? 1 : 0;
      PumpMessages();
      result->commit = store->text();
      result->document_after_action = result->commit;
      result->candidates_after_action =
          ReadCandidateList(interfaces.behavior.get());
      result->candidates_final = result->candidates_after_action;
      result->watches.push_back(
          WatchWindow("after_integratable_key", runtime_pid));
      finish_visibility();
      return;
    }

    if (mode == Mode::BehaviorInvalid && interfaces.behavior) {
      result->behavior.set_selection_called = true;
      result->behavior.set_selection_result =
          interfaces.behavior->SetSelection(result->candidates.count);
      PumpMessages();
      result->document_after_action = store->text();
      result->candidates_after_action =
          ReadCandidateList(interfaces.behavior.get());
      if (interfaces.integratable) {
        result->integratable.finalize_exact_called = true;
        result->integratable.finalize_exact_result =
            interfaces.integratable->FinalizeExactCompositionString();
      }
      PumpMessages();
      result->commit = store->text();
      result->candidates_final = ReadCandidateList(interfaces.behavior.get());
      result->watches.push_back(
          WatchWindow("after_invalid_then_exact_finalize", runtime_pid));
      finish_visibility();
      return;
    }

    Fail("candidate_control_interface",
         "the requested candidate-control interface was unavailable");
    result->watches.push_back(
        WatchWindow("after_missing_candidate_control", runtime_pid));
    finish_visibility();
    return;
  }

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
  finish_visibility();
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
  if (!runtime.Start(staged.runtime(), suffix, staged.data_root(),
                     staged.lock_root())) {
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
    UiElementSink *sink = new UiElementSink(HostAllowsSelfDraw(mode));

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

  if (mode == Mode::BehaviorSelect) {
    Expect(result.behavior.available, "behavior_available",
           "the UI element did not expose candidate-list behavior");
    Expect(result.behavior.set_selection_called &&
               result.behavior.set_selection_result == S_OK,
           "behavior_set_selection",
           "SetSelection(1) did not succeed");
    Expect(result.candidates_after_action.available &&
               result.candidates_after_action.count == result.candidates.count &&
               result.candidates_after_action.selection == 1,
           "behavior_selection_state",
           "SetSelection(1) did not retain the candidate list at selection 1");
    Expect(result.document_after_action.empty(), "behavior_select_no_commit",
           "SetSelection committed text before Finalize");
  } else if (mode == Mode::BehaviorFinalize) {
    Expect(result.behavior.available, "behavior_available",
           "the UI element did not expose candidate-list behavior");
    Expect(result.behavior.set_selection_called &&
               result.behavior.set_selection_result == S_OK &&
               result.candidates_after_action.available &&
               result.candidates_after_action.count == result.candidates.count &&
               result.candidates_after_action.selection == 0,
           "behavior_finalize_selection",
           "SetSelection(0) did not leave candidate 0 selected and active");
    Expect(result.behavior.finalize_called &&
               result.behavior.finalize_result == S_OK,
           "behavior_finalize", "Finalize did not succeed");
    Expect(result.commit == kExpectedCommit, "behavior_finalize_commit",
           "Finalize did not commit U+4F60 U+597D");
    Expect(result.candidates_final.available &&
               result.candidates_final.count == 0,
           "behavior_finalize_end",
           "Finalize did not end the candidate composition");
  } else if (mode == Mode::BehaviorAbort) {
    Expect(result.behavior.available, "behavior_available",
           "the UI element did not expose candidate-list behavior");
    Expect(result.behavior.abort_called && result.behavior.abort_result == S_OK,
           "behavior_abort", "Abort did not succeed");
    Expect(result.document_after_action.empty(), "behavior_abort_no_commit",
           "Abort committed text instead of clearing the composition");
    Expect(result.candidates_final.available &&
               result.candidates_final.count == 0,
           "behavior_abort_end", "Abort left the candidate composition active");
  } else if (mode == Mode::IntegratableKeys) {
    Expect(result.behavior.available && result.integratable.available,
           "integratable_available",
           "the UI element did not expose both candidate-list interfaces");
    Expect(result.integratable.distinct_interface_addresses,
           "integratable_distinct_interface_addresses",
           "Behavior and Integratable interface addresses unexpectedly matched");
    Expect(result.integratable.selection_style_called &&
               result.integratable.selection_style_result == S_OK &&
               result.integratable.selection_style == STYLE_ACTIVE_SELECTION,
           "integratable_selection_style",
           "GetSelectionStyle did not report STYLE_ACTIVE_SELECTION");
    Expect(result.integratable.show_numbers_called &&
               result.integratable.show_numbers_result == S_OK &&
               result.integratable.show_numbers == 1,
           "integratable_candidate_numbers",
           "ShowCandidateNumbers did not ask the host to draw numbers");
    Expect(result.integratable.key_called &&
               result.integratable.key_result == S_OK &&
               result.integratable.key_eaten == 1,
           "integratable_key_eaten",
           "OnKeyDown('1') did not consume the candidate-selection key");
    Expect(result.commit == kExpectedCommit, "integratable_key_commit",
           "OnKeyDown('1') did not commit U+4F60 U+597D");
    Expect(result.candidates_final.available &&
               result.candidates_final.count == 0,
           "integratable_key_end",
           "the integrated selection key did not end the composition");
  } else if (mode == Mode::BehaviorInvalid) {
    Expect(result.behavior.available && result.integratable.available,
           "candidate_control_available",
           "the UI element did not expose both candidate-list interfaces");
    Expect(result.behavior.set_selection_called &&
               result.behavior.set_selection_result == E_INVALIDARG,
           "behavior_invalid_rejected",
           "an out-of-range SetSelection did not return E_INVALIDARG");
    Expect(result.candidates_after_action.available &&
               result.candidates_after_action.count == result.candidates.count &&
               result.candidates_after_action.selection ==
                   result.candidates.selection &&
               result.candidates_after_action.selected ==
                   result.candidates.selected,
           "behavior_invalid_unchanged",
           "an invalid SetSelection changed the candidate list");
    Expect(result.document_after_action.empty(), "behavior_invalid_no_commit",
           "an invalid SetSelection committed text");
    Expect(result.integratable.finalize_exact_called &&
               result.integratable.finalize_exact_result == S_OK,
           "integratable_finalize_exact",
           "FinalizeExactCompositionString did not succeed after rejection");
    Expect(result.commit == L"nihao",
           "integratable_finalize_exact_commit",
           "FinalizeExactCompositionString did not commit raw nihao preedit");
    Expect(result.candidates_final.available &&
               result.candidates_final.count == 0,
           "integratable_finalize_exact_end",
           "FinalizeExactCompositionString did not end the composition");
  } else {
    Expect(result.commit == kExpectedCommit, "commit_text",
           "the composition did not commit U+4F60 U+597D");
  }

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
  if (ExercisesCandidateControl(mode)) {
    Expect(result.candidates.count == 2, "deterministic_candidates",
           "the test engine did not publish the two expected nihao candidates");
  }

  const Watch *candidates = watch("candidates");
  if (!candidates) {
    Fail("candidates_watch", "the candidate stage was never observed");
    return;
  }
  if (!HostAllowsSelfDraw(mode)) {
    Expect(!candidates->ever_visible, "window_denied",
           "the self-drawn candidate window appeared although the host answered "
           "pbShow=FALSE");
    const Watch *full_drive = watch("full_drive");
    Expect(result.continuous_visibility_started && full_drive &&
               !full_drive->ever_visible,
           "window_denied_full_drive",
           "continuous monitoring did not prove the self-drawn window stayed "
           "hidden throughout the key and candidate-control calls");
  } else {
    Expect(candidates->ever_visible, "window_shown",
           "the self-drawn candidate window never appeared although the host "
           "answered pbShow=TRUE");
    const Watch *full_drive = watch("full_drive");
    Expect(result.continuous_visibility_started && full_drive &&
               full_drive->ever_visible,
           "window_shown_full_drive",
           "the full-drive monitor did not observe the allowed candidate "
           "window positive control");
  }

  if (mode == Mode::ShowFalse) {
    const Watch *hidden = watch("after_show_false");
    Expect(hidden && !hidden->final_visible, "window_hidden_on_show_false",
           "ITfUIElement::Show(FALSE) did not hide the self-drawn candidate "
           "window");
  }

  const char *final_stage = nullptr;
  if (mode == Mode::BehaviorSelect)
    final_stage = "after_behavior_select";
  else if (mode == Mode::BehaviorFinalize)
    final_stage = "after_behavior_finalize";
  else if (mode == Mode::BehaviorAbort)
    final_stage = "after_behavior_abort";
  else if (mode == Mode::IntegratableKeys)
    final_stage = "after_integratable_key";
  else if (mode == Mode::BehaviorInvalid)
    final_stage = "after_invalid_then_exact_finalize";
  else
    final_stage = "after_commit";
  const Watch *finished = watch(final_stage);
  Expect(finished && !finished->final_visible, "window_hidden_after_action",
         "the candidate window was visible after the mode's final action");
  if (!HostAllowsSelfDraw(mode)) {
    Expect(finished && !finished->ever_visible,
           "window_never_shown_after_action",
           "the self-drawn candidate window appeared after the host answered "
           "pbShow=FALSE");
  }
}

void Report(Mode mode, const ProbeResult &result) {
  std::printf("{\n");
  std::printf("  \"probe\": \"uiless_candidate_probe\",\n");
  std::printf("  \"issue\": %d,\n", PrimaryIssue(mode));
  std::printf("  \"covers_issues\": %s,\n", CoveredIssues(mode));
  std::printf("  \"mode\": \"%s\",\n", ModeName(mode));
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
  std::printf("  \"continuous_visibility_started\": %s,\n",
              JsonBool(result.continuous_visibility_started));

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

  std::printf("  \"candidate_list\": {\"available\": %s, "
              "\"get_count_result\": %s, \"get_selection_result\": %s, "
              "\"count\": %u, \"selection\": %u, \"selected\": %s, "
              "\"is_shown\": %s},\n",
              JsonBool(result.candidates.available),
              JsonHresult(result.candidates.count_called,
                          result.candidates.count_result).c_str(),
              JsonHresult(result.candidates.selection_called,
                          result.candidates.selection_result).c_str(),
              result.candidates.count,
              result.candidates.selection,
              JsonString(result.candidates.selected).c_str(),
              JsonTriState(result.candidates.is_shown));

  std::printf(
      "  \"candidate_list_after_action\": {\"available\": %s, "
      "\"get_count_result\": %s, \"get_selection_result\": %s, "
      "\"count\": %u, \"selection\": %u, \"selected\": %s, "
      "\"is_shown\": %s},\n",
      JsonBool(result.candidates_after_action.available),
      JsonHresult(result.candidates_after_action.count_called,
                  result.candidates_after_action.count_result).c_str(),
      JsonHresult(result.candidates_after_action.selection_called,
                  result.candidates_after_action.selection_result).c_str(),
      result.candidates_after_action.count,
      result.candidates_after_action.selection,
      JsonString(result.candidates_after_action.selected).c_str(),
      JsonTriState(result.candidates_after_action.is_shown));
  std::printf(
      "  \"candidate_list_final\": {\"available\": %s, "
      "\"get_count_result\": %s, \"get_selection_result\": %s, "
      "\"count\": %u, \"selection\": %u, \"selected\": %s, "
      "\"is_shown\": %s},\n",
      JsonBool(result.candidates_final.available),
      JsonHresult(result.candidates_final.count_called,
                  result.candidates_final.count_result).c_str(),
      JsonHresult(result.candidates_final.selection_called,
                  result.candidates_final.selection_result).c_str(),
      result.candidates_final.count, result.candidates_final.selection,
      JsonString(result.candidates_final.selected).c_str(),
      JsonTriState(result.candidates_final.is_shown));
  std::printf(
      "  \"behavior\": {\"available\": %s, \"set_selection_result\": "
      "%s, \"finalize_result\": %s, \"abort_result\": %s},\n",
      JsonBool(result.behavior.available),
      JsonHresult(result.behavior.set_selection_called,
                  result.behavior.set_selection_result)
          .c_str(),
      JsonHresult(result.behavior.finalize_called,
                  result.behavior.finalize_result)
          .c_str(),
      JsonHresult(result.behavior.abort_called, result.behavior.abort_result)
          .c_str());
  std::printf(
      "  \"integratable\": {\"available\": %s, "
      "\"distinct_interface_addresses\": %s, "
      "\"selection_style_result\": %s, \"selection_style\": %s, "
      "\"show_numbers_result\": %s, \"show_numbers\": %s, "
      "\"key_result\": %s, \"key_eaten\": %s, "
      "\"finalize_exact_result\": %s},\n",
      JsonBool(result.integratable.available),
      JsonBool(result.integratable.distinct_interface_addresses),
      JsonHresult(result.integratable.selection_style_called,
                  result.integratable.selection_style_result)
          .c_str(),
      JsonNumber(result.integratable.selection_style_called,
                 static_cast<long>(result.integratable.selection_style))
          .c_str(),
      JsonHresult(result.integratable.show_numbers_called,
                  result.integratable.show_numbers_result)
          .c_str(),
      JsonTriState(result.integratable.show_numbers),
      JsonHresult(result.integratable.key_called, result.integratable.key_result)
          .c_str(),
      JsonTriState(result.integratable.key_eaten),
      JsonHresult(result.integratable.finalize_exact_called,
                  result.integratable.finalize_exact_result)
          .c_str());
  std::printf("  \"document_after_action\": %s,\n",
              JsonString(result.document_after_action).c_str());

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
      else if (value == L"behavior-select")
        mode = Mode::BehaviorSelect;
      else if (value == L"behavior-finalize")
        mode = Mode::BehaviorFinalize;
      else if (value == L"behavior-abort")
        mode = Mode::BehaviorAbort;
      else if (value == L"integratable-keys")
        mode = Mode::IntegratableKeys;
      else if (value == L"behavior-invalid")
        mode = Mode::BehaviorInvalid;
      else
        mode_set = false;
    } else {
      mode_set = false;
      break;
    }
  }
  if (!mode_set) {
    std::fprintf(stderr,
                 "usage: uiless_candidate_probe --mode "
                 "allow|deny|show-false|behavior-select|behavior-finalize|"
                 "behavior-abort|integratable-keys|behavior-invalid\n");
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
