#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include "candidate_skin.h"
#include "candidate_window.h"

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #value, __FILE__,   \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

using namespace famo::runtime;
using Microsoft::WRL::ComPtr;

namespace {

HANDLE g_preview_received_event = nullptr;
HANDLE g_fake_preview_received_event = nullptr;
HWND g_expected_preview_source = nullptr;
PreviewSelectionRequest g_expected_preview_request{};

struct WinEventRecord {
  DWORD event = 0;
  HWND window = nullptr;
  LONG object_id = 0;
  LONG child_id = 0;
};

std::vector<WinEventRecord> *g_candidate_win_events = nullptr;

void CALLBACK RecordCandidateWinEvent(HWINEVENTHOOK, DWORD event, HWND window,
                                      LONG object_id, LONG child_id, DWORD,
                                      DWORD) {
  if (!g_candidate_win_events)
    return;
  g_candidate_win_events->push_back({event, window, object_id, child_id});
}

class CandidateWinEventRecorder {
public:
  ~CandidateWinEventRecorder() {
    if (hook_)
      UnhookWinEvent(hook_);
    if (g_candidate_win_events == &events_)
      g_candidate_win_events = nullptr;
  }

  bool Start() {
    if (g_candidate_win_events)
      return false;
    g_candidate_win_events = &events_;
    hook_ = SetWinEventHook(EVENT_OBJECT_IME_SHOW, EVENT_OBJECT_IME_CHANGE,
                            nullptr, &RecordCandidateWinEvent,
                            GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    if (hook_)
      return true;
    g_candidate_win_events = nullptr;
    return false;
  }

  bool WaitForCount(DWORD event, size_t expected) {
    for (int attempt = 0; attempt < 500; ++attempt) {
      PumpMessages();
      if (Count(event) >= expected)
        return true;
      Sleep(1);
    }
    return false;
  }

  size_t Count(DWORD event) const {
    return static_cast<size_t>(std::count_if(
        events_.begin(), events_.end(),
        [event](const WinEventRecord &record) { return record.event == event; }));
  }

  void Settle() {
    for (int attempt = 0; attempt < 50; ++attempt) {
      PumpMessages();
      Sleep(1);
    }
    PumpMessages();
  }

  void Clear() {
    PumpMessages();
    events_.clear();
  }

  bool AllAreClientSelf(DWORD event) const {
    return std::all_of(events_.begin(), events_.end(),
                       [event](const WinEventRecord &record) {
                         return record.event != event ||
                                (record.window != nullptr &&
                                 record.object_id == OBJID_CLIENT &&
                                 record.child_id == CHILDID_SELF);
                       });
  }

private:
  static void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  HWINEVENTHOOK hook_ = nullptr;
  std::vector<WinEventRecord> events_;
};

struct AutomationEventCounts {
  volatile LONG opened = 0;
  volatile LONG closed = 0;
  volatile LONG selected = 0;
  volatile LONG layout_invalidated = 0;
  volatile LONG selection_invalidated = 0;
  volatile LONG bounds_changed = 0;
  volatile LONG offscreen_changed = 0;
  volatile LONG selection_property_changed = 0;
  volatile LONG structure_changed = 0;
};

class AutomationEventRecorder final : public IUIAutomationEventHandler {
public:
  AutomationEventRecorder(AutomationEventCounts *counts, HANDLE opened,
                          HANDLE closed, HANDLE selected)
      : counts_(counts), opened_(opened), closed_(closed),
        selected_(selected) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IUIAutomationEventHandler)
      *object = static_cast<IUIAutomationEventHandler *>(this);
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

  HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *,
                                                   EVENTID event) override {
    if (!counts_)
      return S_OK;
    if (event == UIA_MenuOpenedEventId) {
      InterlockedIncrement(&counts_->opened);
      SetEvent(opened_);
    } else if (event == UIA_MenuClosedEventId) {
      InterlockedIncrement(&counts_->closed);
      SetEvent(closed_);
    } else if (event == UIA_SelectionItem_ElementSelectedEventId) {
      InterlockedIncrement(&counts_->selected);
      SetEvent(selected_);
    } else if (event == UIA_LayoutInvalidatedEventId) {
      InterlockedIncrement(&counts_->layout_invalidated);
    } else if (event == UIA_Selection_InvalidatedEventId) {
      InterlockedIncrement(&counts_->selection_invalidated);
    }
    return S_OK;
  }

private:
  ~AutomationEventRecorder() = default;

  std::atomic<ULONG> references_{1};
  AutomationEventCounts *counts_ = nullptr;
  HANDLE opened_ = nullptr;
  HANDLE closed_ = nullptr;
  HANDLE selected_ = nullptr;
};

class AutomationPropertyRecorder final
    : public IUIAutomationPropertyChangedEventHandler {
public:
  explicit AutomationPropertyRecorder(AutomationEventCounts *counts)
      : counts_(counts) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown ||
        iid == IID_IUIAutomationPropertyChangedEventHandler) {
      *object = static_cast<IUIAutomationPropertyChangedEventHandler *>(this);
    }
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

  HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(
      IUIAutomationElement *, PROPERTYID property, VARIANT) override {
    if (!counts_)
      return S_OK;
    if (property == UIA_BoundingRectanglePropertyId)
      InterlockedIncrement(&counts_->bounds_changed);
    else if (property == UIA_IsOffscreenPropertyId)
      InterlockedIncrement(&counts_->offscreen_changed);
    else if (property == UIA_SelectionItemIsSelectedPropertyId)
      InterlockedIncrement(&counts_->selection_property_changed);
    return S_OK;
  }

private:
  ~AutomationPropertyRecorder() = default;
  std::atomic<ULONG> references_{1};
  AutomationEventCounts *counts_ = nullptr;
};

class AutomationStructureRecorder final
    : public IUIAutomationStructureChangedEventHandler {
public:
  explicit AutomationStructureRecorder(AutomationEventCounts *counts)
      : counts_(counts) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown ||
        iid == IID_IUIAutomationStructureChangedEventHandler) {
      *object = static_cast<IUIAutomationStructureChangedEventHandler *>(this);
    }
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

  HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(
      IUIAutomationElement *, StructureChangeType, SAFEARRAY *) override {
    if (counts_)
      InterlockedIncrement(&counts_->structure_changed);
    return S_OK;
  }

private:
  ~AutomationStructureRecorder() = default;
  std::atomic<ULONG> references_{1};
  AutomationEventCounts *counts_ = nullptr;
};

class ScopedCom {
public:
  ScopedCom() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ScopedCom() {
    if (SUCCEEDED(result_))
      CoUninitialize();
  }
  HRESULT result() const { return result_; }

private:
  HRESULT result_;
};

uintptr_t ParseUintPtr(const wchar_t *value) {
  return static_cast<uintptr_t>(_wcstoui64(value, nullptr, 10));
}

int RunAutomationListener(int argc, wchar_t **argv) {
  if (argc != 9)
    return 2;
  const HWND window = reinterpret_cast<HWND>(ParseUintPtr(argv[2]));
  const HANDLE ready = reinterpret_cast<HANDLE>(ParseUintPtr(argv[3]));
  const HANDLE opened = reinterpret_cast<HANDLE>(ParseUintPtr(argv[4]));
  const HANDLE closed = reinterpret_cast<HANDLE>(ParseUintPtr(argv[5]));
  const HANDLE selected = reinterpret_cast<HANDLE>(ParseUintPtr(argv[6]));
  const HANDLE stop = reinterpret_cast<HANDLE>(ParseUintPtr(argv[7]));
  const HANDLE mapping = reinterpret_cast<HANDLE>(ParseUintPtr(argv[8]));
  auto *counts = static_cast<AutomationEventCounts *>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(AutomationEventCounts)));
  if (!window || !ready || !opened || !closed || !selected || !stop ||
      !mapping || !counts) {
    return 3;
  }

  ScopedCom com;
  if (FAILED(com.result()))
    return 4;
  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(automation.GetAddressOf())))) {
    return 5;
  }
  ComPtr<IUIAutomationElement> root;
  if (FAILED(automation->ElementFromHandle(window, root.GetAddressOf())))
    return 6;
  auto *recorder =
      new AutomationEventRecorder(counts, opened, closed, selected);
  ComPtr<IUIAutomationEventHandler> events;
  events.Attach(recorder);
  ComPtr<IUIAutomationPropertyChangedEventHandler> properties;
  properties.Attach(new AutomationPropertyRecorder(counts));
  ComPtr<IUIAutomationStructureChangedEventHandler> structure;
  structure.Attach(new AutomationStructureRecorder(counts));
  PROPERTYID observed_properties[] = {
      UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId,
      UIA_SelectionItemIsSelectedPropertyId};
  if (FAILED(automation->AddAutomationEventHandler(
          UIA_MenuOpenedEventId, root.Get(), TreeScope_Element, nullptr,
          events.Get())) ||
      FAILED(automation->AddAutomationEventHandler(
          UIA_MenuClosedEventId, root.Get(), TreeScope_Element, nullptr,
          events.Get())) ||
      FAILED(automation->AddAutomationEventHandler(
          UIA_SelectionItem_ElementSelectedEventId, root.Get(),
          TreeScope_Subtree, nullptr, events.Get())) ||
      FAILED(automation->AddAutomationEventHandler(
          UIA_LayoutInvalidatedEventId, root.Get(), TreeScope_Element, nullptr,
          events.Get())) ||
      FAILED(automation->AddAutomationEventHandler(
          UIA_Selection_InvalidatedEventId, root.Get(), TreeScope_Element,
          nullptr, events.Get())) ||
      FAILED(automation->AddPropertyChangedEventHandlerNativeArray(
          root.Get(), TreeScope_Subtree, nullptr, properties.Get(),
          observed_properties,
          static_cast<int>(std::size(observed_properties)))) ||
      FAILED(automation->AddStructureChangedEventHandler(
          root.Get(), TreeScope_Element, nullptr, structure.Get()))) {
    return 7;
  }
  SetEvent(ready);
  const DWORD wait = WaitForSingleObject(stop, 30000);
  const bool removed =
      SUCCEEDED(automation->RemoveAutomationEventHandler(
          UIA_MenuOpenedEventId, root.Get(), events.Get())) &&
      SUCCEEDED(automation->RemoveAutomationEventHandler(
          UIA_MenuClosedEventId, root.Get(), events.Get())) &&
      SUCCEEDED(automation->RemoveAutomationEventHandler(
          UIA_SelectionItem_ElementSelectedEventId, root.Get(),
          events.Get())) &&
      SUCCEEDED(automation->RemoveAutomationEventHandler(
          UIA_LayoutInvalidatedEventId, root.Get(), events.Get())) &&
      SUCCEEDED(automation->RemoveAutomationEventHandler(
          UIA_Selection_InvalidatedEventId, root.Get(), events.Get())) &&
      SUCCEEDED(automation->RemovePropertyChangedEventHandler(
          root.Get(), properties.Get())) &&
      SUCCEEDED(automation->RemoveStructureChangedEventHandler(
          root.Get(), structure.Get()));
  UnmapViewOfFile(counts);
  return wait == WAIT_OBJECT_0 && removed ? 0 : 8;
}

class AutomationListenerProcess {
public:
  ~AutomationListenerProcess() { (void)Finish(); }

  bool Start(HWND window) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    ready_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    opened_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    closed_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    selected_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    stop_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
                                  PAGE_READWRITE, 0,
                                  sizeof(AutomationEventCounts), nullptr);
    counts_ = mapping_ ? static_cast<AutomationEventCounts *>(MapViewOfFile(
                            mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                            sizeof(AutomationEventCounts)))
                       : nullptr;
    if (!ready_ || !opened_ || !closed_ || !selected_ || !stop_ ||
        !mapping_ || !counts_) {
      return false;
    }
    *counts_ = {};
    wchar_t module[32768]{};
    if (GetModuleFileNameW(nullptr, module,
                           static_cast<DWORD>(std::size(module))) == 0) {
      return false;
    }
    const auto number = [](const void *value) {
      return std::to_wstring(reinterpret_cast<uintptr_t>(value));
    };
    std::wstring command =
        L"\"" + std::wstring(module) + L"\" --uia-listener " +
        number(window) + L" " + number(ready_) + L" " + number(opened_) +
        L" " + number(closed_) + L" " + number(selected_) + L" " +
        number(stop_) + L" " + number(mapping_);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process_)) {
      return false;
    }
    return WaitForSingleObject(ready_, 3000) == WAIT_OBJECT_0;
  }

  bool Wait(EVENTID event) const {
    HANDLE signal = nullptr;
    if (event == UIA_MenuOpenedEventId)
      signal = opened_;
    else if (event == UIA_MenuClosedEventId)
      signal = closed_;
    else if (event == UIA_SelectionItem_ElementSelectedEventId)
      signal = selected_;
    return signal && WaitForSingleObject(signal, 3000) == WAIT_OBJECT_0;
  }

  LONG Count(EVENTID event) const {
    if (!counts_)
      return 0;
    const volatile LONG *value = nullptr;
    if (event == UIA_MenuOpenedEventId)
      value = &counts_->opened;
    else if (event == UIA_MenuClosedEventId)
      value = &counts_->closed;
    else if (event == UIA_SelectionItem_ElementSelectedEventId)
      value = &counts_->selected;
    else if (event == UIA_LayoutInvalidatedEventId)
      value = &counts_->layout_invalidated;
    else if (event == UIA_Selection_InvalidatedEventId)
      value = &counts_->selection_invalidated;
    return value ? InterlockedCompareExchange(
                       const_cast<volatile LONG *>(value), 0, 0)
                 : 0;
  }

  LONG PropertyCount(PROPERTYID property) const {
    if (!counts_)
      return 0;
    const volatile LONG *value = nullptr;
    if (property == UIA_BoundingRectanglePropertyId)
      value = &counts_->bounds_changed;
    else if (property == UIA_IsOffscreenPropertyId)
      value = &counts_->offscreen_changed;
    else if (property == UIA_SelectionItemIsSelectedPropertyId)
      value = &counts_->selection_property_changed;
    return value ? InterlockedCompareExchange(
                       const_cast<volatile LONG *>(value), 0, 0)
                 : 0;
  }

  LONG StructureCount() const {
    return counts_ ? InterlockedCompareExchange(
                         &counts_->structure_changed, 0, 0)
                   : 0;
  }

  bool Finish() {
    bool success = true;
    if (process_.hProcess) {
      SetEvent(stop_);
      const DWORD wait = WaitForSingleObject(process_.hProcess, 5000);
      DWORD exit_code = STILL_ACTIVE;
      GetExitCodeProcess(process_.hProcess, &exit_code);
      success = wait == WAIT_OBJECT_0 && exit_code == 0;
    }
    if (process_.hThread)
      CloseHandle(process_.hThread);
    if (process_.hProcess)
      CloseHandle(process_.hProcess);
    process_ = {};
    if (counts_)
      UnmapViewOfFile(counts_);
    counts_ = nullptr;
    const HANDLE handles[] = {ready_, opened_, closed_, selected_, stop_,
                              mapping_};
    for (HANDLE handle : handles) {
      if (handle)
        CloseHandle(handle);
    }
    ready_ = opened_ = closed_ = selected_ = stop_ = mapping_ = nullptr;
    return success;
  }

private:
  PROCESS_INFORMATION process_{};
  HANDLE ready_ = nullptr;
  HANDLE opened_ = nullptr;
  HANDLE closed_ = nullptr;
  HANDLE selected_ = nullptr;
  HANDLE stop_ = nullptr;
  HANDLE mapping_ = nullptr;
  AutomationEventCounts *counts_ = nullptr;
};

LRESULT CALLBACK PreviewTargetProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  if (message == WM_COPYDATA) {
    const auto *copy = reinterpret_cast<const COPYDATASTRUCT *>(lparam);
    if (!copy || copy->dwData != kPreviewSelectionCopyDataId ||
        copy->cbData != sizeof(PreviewSelectionRequest) || !copy->lpData) {
      return FALSE;
    }
    const auto &request =
        *static_cast<const PreviewSelectionRequest *>(copy->lpData);
    if (reinterpret_cast<HWND>(wparam) != g_expected_preview_source ||
        request.correlation != g_expected_preview_request.correlation ||
        request.composition_sequence !=
            g_expected_preview_request.composition_sequence ||
        request.absolute_index !=
            g_expected_preview_request.absolute_index ||
        request.reserved != 0 ||
        request.selection_capability !=
            g_expected_preview_request.selection_capability) {
      return FALSE;
    }
    SetEvent(g_preview_received_event);
    return TRUE;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK FakePreviewTargetProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
  if (message == WM_COPYDATA) {
    SetEvent(g_fake_preview_received_event);
    return TRUE;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

PipeClientIdentity ProcessIdentity(HANDLE process, DWORD process_id) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!process ||
      !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return {};
  }
  ULARGE_INTEGER encoded{};
  encoded.LowPart = created.dwLowDateTime;
  encoded.HighPart = created.dwHighDateTime;
  return {process_id, encoded.QuadPart};
}

PipeClientIdentity CurrentProcessIdentity() {
  return ProcessIdentity(GetCurrentProcess(), GetCurrentProcessId());
}

int RunFakePreviewTarget(int argc, wchar_t **argv) {
  if (argc != 6)
    return 2;
  HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
  g_fake_preview_received_event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
  HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[5]);
  if (!ready || !g_fake_preview_received_event || !stop)
    return 3;
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = FakePreviewTargetProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kPreviewSelectionWindowClass;
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 4;
  }
  HWND target =
      CreateWindowExW(0, kPreviewSelectionWindowClass, argv[2], 0, 0, 0, 0, 0,
                      HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!target)
    return 5;
  SetEvent(ready);
  bool running = true;
  while (running) {
    const DWORD wait =
        MsgWaitForMultipleObjects(1, &stop, FALSE, 1000, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) {
      running = false;
    } else if (wait == WAIT_OBJECT_0 + 1) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    } else if (wait == WAIT_FAILED) {
      running = false;
    }
  }
  DestroyWindow(target);
  CloseHandle(stop);
  CloseHandle(g_fake_preview_received_event);
  CloseHandle(ready);
  return 0;
}

std::shared_ptr<RuntimeSnapshot> VisibleSnapshot(uint64_t sequence = 3) {
  auto snapshot = std::make_shared<RuntimeSnapshot>();
  snapshot->correlation = {1, 2, 3, 4, 5, sequence};
  snapshot->composition.handled = true;
  snapshot->composition.preedit = "ni";
  snapshot->composition.candidates = {
      Candidate{"\xe4\xbd\xa0", "", "1", 0, 0},
      Candidate{"\xe5\xb0\xbc", "", "2", 0, 0}};
  snapshot->composition.page_size = 8;
  snapshot->composition.is_last_page = 1;
  snapshot->ui_state = {{400, 300, 402, 320},
                        {-1920, 0, 1920, 1080},
                        192,
                        true,
                        true,
                        true,
                        {sequence, sequence ^ 0xfeedbeefull}};
  snapshot->composition_sequence = sequence - 1;
  snapshot->ui_sequence = sequence;
  return snapshot;
}

struct WindowProbe {
  HWND window = nullptr;
  RECT rect{};
  LONG_PTR ex_style = 0;
  HCURSOR cursor = nullptr;
  bool visible = false;
};

BOOL CALLBACK FindCandidate(HWND window, LPARAM parameter) {
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != GetCurrentProcessId())
    return TRUE;
  wchar_t name[64]{};
  GetClassNameW(window, name, static_cast<int>(std::size(name)));
  if (std::wstring_view(name) != L"FamoRuntimeCandidateWindow")
    return TRUE;
  auto *probe = reinterpret_cast<WindowProbe *>(parameter);
  const bool visible = IsWindowVisible(window) != FALSE;
  if (!probe->window || (!probe->visible && visible)) {
    probe->window = window;
    probe->visible = visible;
    probe->ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    probe->cursor =
        reinterpret_cast<HCURSOR>(GetClassLongPtrW(window, GCLP_HCURSOR));
    GetWindowRect(window, &probe->rect);
  }
  return visible ? FALSE : TRUE;
}

WindowProbe Probe() {
  WindowProbe result;
  EnumWindows(&FindCandidate, reinterpret_cast<LPARAM>(&result));
  return result;
}

bool WaitForVisibility(bool visible, WindowProbe *result = nullptr) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    WindowProbe probe = Probe();
    if (probe.window && probe.visible == visible) {
      if (result)
        *result = probe;
      return true;
    }
    Sleep(1);
  }
  return false;
}

bool PreviewRowsMapToAbsoluteCandidateIndexes() {
  FamoLayoutResult layout{};
  layout.preview_candidate_count = 3;
  layout.preview_candidates[0].bounds = {10, 20, 30, 40};
  layout.preview_candidates[1].bounds = {30, 20, 50, 40};
  layout.preview_candidates[2].bounds = {10, 40, 30, 60};
  PreviewSelection selection;
  CHECK(PreviewSelectionAt(layout, 35, 25, 0, 2, &selection));
  CHECK(selection.absolute_index == 3);
  CHECK(PreviewSelectionAt(layout, 15, 45, 0, 2, &selection));
  CHECK(selection.absolute_index == 4);
  CHECK(PreviewSelectionAt(layout, 15, 25, 4, 2, &selection));
  CHECK(selection.absolute_index == 10);
  CHECK(!PreviewSelectionAt(layout, 5, 5, 0, 2, &selection));
  CHECK(!PreviewSelectionAt(layout, 15, 25, 0, 0, &selection));
  return true;
}

bool CandidateWindowUsesExplicitInProcessOwner() {
  HWND owner = CreateWindowExW(0, L"STATIC", L"candidate-owner",
                               WS_OVERLAPPEDWINDOW,
                               0, 0, 640, 480, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  ShowWindow(owner, SW_SHOWNOACTIVATE);
  CHECK(IsWindowVisible(owner));
  CHECK(SetForegroundWindow(owner));
  CHECK(GetForegroundWindow() == owner);

  CandidateWindow candidate;
  CHECK(candidate.Start());
  auto snapshot = VisibleSnapshot(190);
  snapshot->revision = 1;
  snapshot->source_window = reinterpret_cast<uintptr_t>(owner);
  snapshot->require_in_process_owner = true;
  candidate.Publish(snapshot);

  WindowProbe probe;
  CHECK(WaitForVisibility(true, &probe));
  CHECK(GetWindow(probe.window, GW_OWNER) == owner);
  CHECK((probe.ex_style & WS_EX_TOPMOST) == 0);

  auto moved = std::make_shared<RuntimeSnapshot>(*snapshot);
  moved->revision = 2;
  moved->ui_state.caret.left += 40;
  moved->ui_state.caret.right += 40;
  candidate.Publish(moved);
  WindowProbe moved_probe = probe;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    moved_probe = Probe();
    if (moved_probe.visible && moved_probe.rect.left != probe.rect.left)
      break;
    Sleep(1);
  }
  CHECK(moved_probe.visible && moved_probe.rect.left != probe.rect.left);
  CHECK((moved_probe.ex_style & WS_EX_TOPMOST) == 0);

  candidate.Stop();
  CHECK(DestroyWindow(owner));
  return true;
}

bool CandidateWindowReleasesRegisteredClasses() {
  const HINSTANCE module = GetModuleHandleW(nullptr);
  CHECK(module != nullptr);
  (void)UnregisterClassW(L"FamoRuntimeCandidateWindow", module);
  (void)UnregisterClassW(L"FamoRuntimeModeIndicator", module);

  CandidateWindow candidate;
  CHECK(candidate.Start());
  CHECK(candidate.Prewarm());
  candidate.Stop();

  WNDCLASSW registered{};
  CHECK(!GetClassInfoW(module, L"FamoRuntimeCandidateWindow", &registered));
  CHECK(!GetClassInfoW(module, L"FamoRuntimeModeIndicator", &registered));
  return true;
}

bool CandidateWindowFollowsOwnerLifecycle() {
  HWND first_owner = CreateWindowExW(
      0, L"STATIC", L"first-candidate-owner", WS_OVERLAPPEDWINDOW, 0, 0, 640,
      480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(first_owner != nullptr);
  ShowWindow(first_owner, SW_SHOW);
  CHECK(SetForegroundWindow(first_owner));

  CandidateWindow candidate;
  CHECK(candidate.Start());
  auto first = VisibleSnapshot(195);
  first->revision = 1;
  first->source_window = reinterpret_cast<uintptr_t>(first_owner);
  first->require_in_process_owner = true;
  candidate.Publish(first);
  WindowProbe first_probe;
  CHECK(WaitForVisibility(true, &first_probe));
  CHECK(GetWindow(first_probe.window, GW_OWNER) == first_owner);

  CHECK(DestroyWindow(first_owner));
  for (int attempt = 0; attempt < 1000 && IsWindow(first_probe.window);
       ++attempt) {
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    Sleep(1);
  }
  CHECK(!IsWindow(first_probe.window));

  HWND replacement_owner = CreateWindowExW(
      0, L"STATIC", L"replacement-candidate-owner", WS_OVERLAPPEDWINDOW, 0,
      0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(replacement_owner != nullptr);
  ShowWindow(replacement_owner, SW_SHOW);
  CHECK(SetForegroundWindow(replacement_owner));
  auto replacement = VisibleSnapshot(196);
  replacement->revision = 2;
  replacement->source_window =
      reinterpret_cast<uintptr_t>(replacement_owner);
  replacement->require_in_process_owner = true;
  candidate.Publish(replacement);
  WindowProbe replacement_probe;
  CHECK(WaitForVisibility(true, &replacement_probe));
  CHECK(GetWindow(replacement_probe.window, GW_OWNER) == replacement_owner);

  candidate.Stop();
  CHECK(DestroyWindow(replacement_owner));
  return true;
}

std::string Utf8Path(std::wstring_view path) {
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
      static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    return {};
  std::string result(static_cast<size_t>(count), '\0');
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
                             static_cast<int>(path.size()), result.data(),
                             count, nullptr, nullptr) == count
             ? result
             : std::string{};
}

// The presenter takes its appearance from the snapshot the Runtime publishes
// and never reads the user's data root itself: it is loaded into hosts, some
// sandboxed, that cannot resolve or open that root at all.
std::shared_ptr<const RuntimeStyleState> StyleWithMargin(int margin_x) {
  const std::string overlay =
      "style:\n  margin_x: " + std::to_string(margin_x) + "\n";
  std::shared_ptr<const void> presentation;
  if (!PrepareCandidateStyle(overlay, true, &presentation))
    return nullptr;
  return std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::move(presentation)});
}

bool CandidateWindowTakesAppearanceFromPublishedStyle() {
  HWND owner = CreateWindowExW(
      0, L"STATIC", L"style-candidate-owner", WS_OVERLAPPEDWINDOW, 0, 0, 640,
      480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(owner != nullptr);
  ShowWindow(owner, SW_SHOW);
  CHECK(SetForegroundWindow(owner));

  CandidateWindow candidate;
  CHECK(candidate.Start());
  auto snapshot = VisibleSnapshot(198);
  snapshot->revision = 1;
  snapshot->source_window = reinterpret_cast<uintptr_t>(owner);
  snapshot->require_in_process_owner = true;
  snapshot->style = StyleWithMargin(2);
  CHECK(snapshot->style != nullptr);
  candidate.Publish(snapshot);
  WindowProbe initial;
  CHECK(WaitForVisibility(true, &initial));
  const LONG initial_width = initial.rect.right - initial.rect.left;
  CHECK(initial_width > 0);

  auto restyled = std::make_shared<RuntimeSnapshot>(*snapshot);
  restyled->revision = 2;
  restyled->style = StyleWithMargin(40);
  CHECK(restyled->style != nullptr);
  candidate.Publish(restyled);

  WindowProbe reloaded = initial;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    reloaded = Probe();
    if (reloaded.visible &&
        reloaded.rect.right - reloaded.rect.left > initial_width) {
      break;
    }
    Sleep(5);
  }
  CHECK(reloaded.visible);
  CHECK(reloaded.rect.right - reloaded.rect.left > initial_width);

  candidate.Stop();
  CHECK(DestroyWindow(owner));
  return true;
}

bool PreviewRoutingSupportsInProcessTarget() {
  WNDCLASSW target_class{};
  target_class.lpfnWndProc = PreviewTargetProc;
  target_class.hInstance = GetModuleHandleW(nullptr);
  target_class.lpszClassName = kPreviewSelectionWindowClass;
  CHECK(RegisterClassW(&target_class) ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

  HWND source = CreateWindowExW(0, L"STATIC", L"candidate-source", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  HWND target = CreateWindowExW(
      0, kPreviewSelectionWindowClass, L"in-process-target", 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(source && target);
  g_preview_received_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  CHECK(g_preview_received_event);
  g_expected_preview_source = source;
  g_expected_preview_request = {
      {101, 102, 103, 104, 105, 106}, 100, 1, 0, {107, 108}};

  CHECK(SendPreviewSelection(source, g_expected_preview_request, target, {}));
  CHECK(WaitForSingleObject(g_preview_received_event, 0) == WAIT_OBJECT_0);

  CloseHandle(g_preview_received_event);
  g_preview_received_event = nullptr;
  g_expected_preview_source = nullptr;
  DestroyWindow(target);
  DestroyWindow(source);
  return true;
}

bool PreviewRoutingBindsExactOwnerAndSourceWindow() {
  CandidateWindow candidate;
  CHECK(candidate.Start());
  auto snapshot = VisibleSnapshot(200);
  snapshot->revision = 1;
  snapshot->correlation.client_id =
      0x70000000ull + static_cast<uint64_t>(GetCurrentProcessId());
  snapshot->selection_owner = CurrentProcessIdentity();
  CHECK(snapshot->selection_owner);
  candidate.Publish(snapshot);
  WindowProbe source;
  CHECK(WaitForVisibility(true, &source));

  const std::wstring nonce =
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64());
  const std::wstring ready_name = L"Local\\FamoPreviewReady-" + nonce;
  const std::wstring fake_hit_name = L"Local\\FamoPreviewHit-" + nonce;
  const std::wstring stop_name = L"Local\\FamoPreviewStop-" + nonce;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
  HANDLE fake_hit =
      CreateEventW(nullptr, TRUE, FALSE, fake_hit_name.c_str());
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
  CHECK(ready && fake_hit && stop);

  wchar_t module[32768]{};
  CHECK(GetModuleFileNameW(nullptr, module,
                           static_cast<DWORD>(std::size(module))) > 0);
  const std::wstring title =
      std::to_wstring(snapshot->correlation.client_id);
  std::wstring command =
      L"\"" + std::wstring(module) + L"\" --fake-preview-target " + title +
      L" " + ready_name + L" " + fake_hit_name + L" " + stop_name;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION fake{};
  CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &fake));
  const PipeClientIdentity fake_identity =
      ProcessIdentity(fake.hProcess, fake.dwProcessId);
  CHECK(fake_identity);
  CHECK(WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0);

  WNDCLASSW target_class{};
  target_class.lpfnWndProc = PreviewTargetProc;
  target_class.hInstance = GetModuleHandleW(nullptr);
  target_class.lpszClassName = kPreviewSelectionWindowClass;
  CHECK(RegisterClassW(&target_class) ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
  HWND target = CreateWindowExW(
      0, kPreviewSelectionWindowClass, title.c_str(), 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(target);
  g_preview_received_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  CHECK(g_preview_received_event);
  g_expected_preview_source = source.window;
  g_expected_preview_request = {
      snapshot->correlation,
      snapshot->composition_sequence,
      7,
      0,
      snapshot->ui_state.selection_capability,
  };

  CHECK(SendPreviewSelectionToOwner(
      source.window, g_expected_preview_request, snapshot->selection_owner));
  CHECK(WaitForSingleObject(g_preview_received_event, 0) == WAIT_OBJECT_0);
  CHECK(WaitForSingleObject(fake_hit, 0) == WAIT_TIMEOUT);

  ResetEvent(g_preview_received_event);
  PipeClientIdentity wrong_creation = snapshot->selection_owner;
  ++wrong_creation.process_creation_time;
  CHECK(!SendPreviewSelectionToOwner(
      source.window, g_expected_preview_request, wrong_creation));
  CHECK(WaitForSingleObject(g_preview_received_event, 0) == WAIT_TIMEOUT);

  SetEvent(stop);
  CHECK(WaitForSingleObject(fake.hProcess, 2000) == WAIT_OBJECT_0);
  CHECK(!SendPreviewSelectionToOwner(
      source.window, g_expected_preview_request, fake_identity));

  DestroyWindow(target);
  CloseHandle(g_preview_received_event);
  g_preview_received_event = nullptr;
  g_expected_preview_source = nullptr;
  CloseHandle(fake.hThread);
  CloseHandle(fake.hProcess);
  CloseHandle(stop);
  CloseHandle(fake_hit);
  CloseHandle(ready);
  candidate.Stop();
  return true;
}

bool ScrollTransitionIsBoundedAndOptional() {
  FamoLayoutResult previous{};
  previous.content_size = {240, 96};
  previous.shadow_margin = 12;
  previous.candidate_count = 2;
  previous.candidates[0].bounds = {8, 15, 58, 35};
  previous.candidates[1].bounds = {64, 15, 114, 35};
  previous.highlight = {8, 11, 58, 37};
  previous.preview_candidate_count = 4;
  previous.preview_candidates[0].bounds = {8, 40, 58, 60};
  previous.preview_candidates[1].bounds = {64, 40, 114, 60};
  previous.preview_candidates[2].bounds = {8, 65, 58, 85};
  previous.preview_candidates[3].bounds = {64, 65, 114, 85};
  FamoLayoutResult next = previous;

  ScrollTransitionPlan plan;
  CHECK(PlanScrollTransition(previous, next, 4, 5, true, &plan));
  CHECK(plan.direction == 1);
  CHECK(plan.row_step == 25);
  CHECK(plan.clip.left == 0 && plan.clip.top == 11 &&
        plan.clip.right == 240 && plan.clip.bottom == 85);
  CHECK(ScrollTransitionOffset(0, plan.row_step) == 0);
  const int32_t halfway = ScrollTransitionOffset(
      kCandidateScrollTransitionMs / 2, plan.row_step);
  CHECK(halfway > 0 && halfway < plan.row_step);
  CHECK(ScrollTransitionOffset(kCandidateScrollTransitionMs, plan.row_step) ==
        plan.row_step);

  CHECK(PlanScrollTransition(previous, next, 5, 4, true, &plan));
  CHECK(plan.direction == -1);
  CHECK(!PlanScrollTransition(previous, next, 4, 5, false, &plan));
  CHECK(!PlanScrollTransition(previous, next, 4, 6, true, &plan));
  next.content_size.cy++;
  CHECK(!PlanScrollTransition(previous, next, 4, 5, true, &plan));
  return true;
}

bool CandidateWindowReportsShowEvent() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto shown = VisibleSnapshot(100);
  shown->revision = 1;
  window.Publish(shown);
  CHECK(WaitForVisibility(true));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 1);
  CHECK(events.AllAreClientSelf(EVENT_OBJECT_IME_SHOW));
  window.Stop();
  return true;
}

template <typename Predicate>
bool WaitForCounters(const CandidateWindow &window, Predicate predicate) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (predicate(window.counters()))
      return true;
    Sleep(1);
  }
  return false;
}

bool CandidateWindowReportsMoveEvent() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto initial = VisibleSnapshot(110);
  initial->revision = 1;
  window.Publish(initial);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  auto moved = VisibleSnapshot(111);
  moved->revision = 2;
  moved->ui_state.selection_capability =
      initial->ui_state.selection_capability;
  moved->ui_state.caret = {700, 450, 702, 470};
  window.Publish(moved);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.anchor_only == 1;
  }));
  WindowProbe moved_probe = Probe();
  CHECK(moved_probe.visible);
  CHECK(moved_probe.rect.left != initial_probe.rect.left ||
        moved_probe.rect.top != initial_probe.rect.top);
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_CHANGE, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 1);
  CHECK(events.AllAreClientSelf(EVENT_OBJECT_IME_CHANGE));
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowRelayoutsWhenWorkAreaShrinks() {
  CandidateWindow window;
  CHECK(window.Start());
  auto initial = VisibleSnapshot(114);
  initial->revision = 1;
  window.Publish(initial);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  const CandidateWindow::Counters before = window.counters();
  const LONG initial_width = initial_probe.rect.right - initial_probe.rect.left;
  CHECK(initial_width > 2);

  auto constrained = std::make_shared<RuntimeSnapshot>(*initial);
  constrained->revision = 2;
  ++constrained->correlation.sequence;
  ++constrained->ui_sequence;
  constrained->ui_state.work_area = {0, 0, initial_width - 1, 1080};
  constrained->ui_state.caret = {initial_width - 4, 300, initial_width - 2,
                                 320};
  window.Publish(constrained);
  CHECK(
      WaitForCounters(window, [before](const CandidateWindow::Counters &value) {
        return value.full > before.full;
      }));
  WindowProbe probe;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    probe = Probe();
    if (probe.visible &&
        probe.rect.left >= constrained->ui_state.work_area.left &&
        probe.rect.top >= constrained->ui_state.work_area.top &&
        probe.rect.right <= constrained->ui_state.work_area.right &&
        probe.rect.bottom <= constrained->ui_state.work_area.bottom) {
      break;
    }
    Sleep(1);
  }
  CHECK(probe.visible);
  CHECK(probe.rect.left >= constrained->ui_state.work_area.left);
  CHECK(probe.rect.top >= constrained->ui_state.work_area.top);
  CHECK(probe.rect.right <= constrained->ui_state.work_area.right);
  CHECK(probe.rect.bottom <= constrained->ui_state.work_area.bottom);
  CHECK(window.counters().anchor_only == before.anchor_only);

  const CandidateWindow::Counters constrained_counters = window.counters();
  auto expanded = std::make_shared<RuntimeSnapshot>(*initial);
  expanded->revision = 3;
  expanded->correlation.sequence = constrained->correlation.sequence + 1;
  expanded->ui_sequence = constrained->ui_sequence + 1;
  window.Publish(expanded);
  CHECK(WaitForCounters(
      window, [constrained_counters](const CandidateWindow::Counters &value) {
        return value.full > constrained_counters.full;
      }));
  WindowProbe expanded_probe;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    expanded_probe = Probe();
    if (expanded_probe.visible &&
        expanded_probe.rect.right - expanded_probe.rect.left >
            probe.rect.right - probe.rect.left) {
      break;
    }
    Sleep(1);
  }
  CHECK(expanded_probe.visible);
  CHECK(expanded_probe.rect.right - expanded_probe.rect.left >
        probe.rect.right - probe.rect.left);
  CHECK(window.counters().anchor_only == constrained_counters.anchor_only);
  window.Stop();
  return true;
}

bool CandidateWindowDoesNotReportNoOpMove() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto initial = VisibleSnapshot(112);
  initial->revision = 1;
  window.Publish(initial);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  auto same_position = VisibleSnapshot(113);
  same_position->revision = 2;
  same_position->ui_state.selection_capability =
      initial->ui_state.selection_capability;
  same_position->ui_state.work_area = {-1921, 0, 1919, 1080};
  window.Publish(same_position);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.anchor_only == 1;
  }));
  WindowProbe same_probe = Probe();
  CHECK(same_probe.visible);
  CHECK(EqualRect(&initial_probe.rect, &same_probe.rect));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowReportsSizeChangeEvent() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto shown = VisibleSnapshot(115);
  shown->revision = 1;
  window.Publish(shown);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  const uint64_t full_before = window.counters().full;
  FamoSkin wide_skin = FamoSkinDefault();
  wide_skin.min_width = 600;
  auto wide_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::make_shared<const CandidateStylePresentation>(
                               CandidateStylePresentation{wide_skin,
                                                          wide_skin})});
  window.ActivateStyle(wide_style);
  CHECK(WaitForCounters(window, [&](const CandidateWindow::Counters &value) {
    return value.full > full_before;
  }));
  WindowProbe resized_probe;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    resized_probe = Probe();
    if (resized_probe.visible &&
        resized_probe.rect.right - resized_probe.rect.left !=
            initial_probe.rect.right - initial_probe.rect.left) {
      break;
    }
    Sleep(1);
  }
  CHECK(resized_probe.visible);
  CHECK(resized_probe.rect.right - resized_probe.rect.left !=
        initial_probe.rect.right - initial_probe.rect.left);
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_CHANGE, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 1);
  CHECK(events.AllAreClientSelf(EVENT_OBJECT_IME_CHANGE));
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowReportsHideEvent() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto shown = VisibleSnapshot(120);
  shown->revision = 1;
  window.Publish(shown);
  CHECK(WaitForVisibility(true));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  auto hidden = VisibleSnapshot(121);
  hidden->revision = 2;
  hidden->ui_state.show_allowed = false;
  window.Publish(hidden);
  CHECK(WaitForVisibility(false));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_HIDE, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 1);
  CHECK(events.AllAreClientSelf(EVENT_OBJECT_IME_HIDE));
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowNeverReportsUnshownWindow() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto hidden = VisibleSnapshot(125);
  hidden->revision = 1;
  hidden->ui_state.show_allowed = false;
  window.Publish(hidden);
  CHECK(window.Prewarm());
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowDoesNotRepeatLightDismissEvents() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto shown = VisibleSnapshot(130);
  shown->revision = 1;
  window.Publish(shown);
  CHECK(WaitForVisibility(true));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  auto repainted = std::make_shared<RuntimeSnapshot>(*shown);
  repainted->revision = 2;
  repainted->correlation.sequence++;
  repainted->composition.highlighted_index = 1;
  window.Publish(repainted);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.selection_only == 1;
  }));
  events.Settle();
  CHECK(Probe().visible);
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);

  auto hidden = std::make_shared<RuntimeSnapshot>(*repainted);
  hidden->revision = 3;
  hidden->correlation.sequence++;
  hidden->ui_state.show_allowed = false;
  window.Publish(hidden);
  CHECK(WaitForVisibility(false));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_HIDE, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 1);
  events.Clear();

  auto still_hidden = std::make_shared<RuntimeSnapshot>(*hidden);
  still_hidden->revision = 4;
  still_hidden->correlation.sequence++;
  window.Publish(still_hidden);
  CHECK(window.Prewarm());
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);
  window.Stop();
  return true;
}

bool CandidateWindowReportsHideWhenStopped() {
  CandidateWinEventRecorder events;
  CHECK(events.Start());
  CandidateWindow window;
  CHECK(window.Start());
  auto shown = VisibleSnapshot(135);
  shown->revision = 1;
  window.Publish(shown);
  CHECK(WaitForVisibility(true));
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_SHOW, 1));
  events.Settle();
  events.Clear();

  window.Stop();
  CHECK(events.WaitForCount(EVENT_OBJECT_IME_HIDE, 1));
  events.Settle();
  CHECK(events.Count(EVENT_OBJECT_IME_HIDE) == 1);
  CHECK(events.AllAreClientSelf(EVENT_OBJECT_IME_HIDE));
  CHECK(events.Count(EVENT_OBJECT_IME_SHOW) == 0);
  CHECK(events.Count(EVENT_OBJECT_IME_CHANGE) == 0);
  return true;
}

bool CandidateWindowExposesAccessibleTreeAndEvents() {
  ScopedCom com;
  CHECK(SUCCEEDED(com.result()));
  ComPtr<IUIAutomation> automation;
  CHECK(SUCCEEDED(CoCreateInstance(
      CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(automation.GetAddressOf()))));

  CandidateWindow window;
  CHECK(window.Start());
  WindowProbe probe;
  CHECK(WaitForVisibility(false, &probe));
  CHECK(probe.window);

  ComPtr<IUIAutomationElement> root;
  CHECK(SUCCEEDED(
      automation->ElementFromHandle(probe.window, root.GetAddressOf())));
  BSTR automation_id = nullptr;
  CHECK(SUCCEEDED(root->get_CurrentAutomationId(&automation_id)) &&
        automation_id &&
        std::wstring_view(automation_id, SysStringLen(automation_id)) ==
            L"IME_Candidate_Window");
  SysFreeString(automation_id);
  CONTROLTYPEID control_type = 0;
  CHECK(SUCCEEDED(root->get_CurrentControlType(&control_type)) &&
        control_type == UIA_ListControlTypeId);

  AutomationListenerProcess listener;
  CHECK(listener.Start(probe.window));

  auto shown = VisibleSnapshot(140);
  shown->revision = 1;
  window.Publish(shown);
  CHECK(WaitForVisibility(true, &probe));
  CHECK(listener.Wait(UIA_MenuOpenedEventId));
  Sleep(25);
  CHECK(listener.Count(UIA_MenuOpenedEventId) == 1);
  CHECK(listener.Count(UIA_SelectionItem_ElementSelectedEventId) == 0);
  CHECK(listener.Count(UIA_LayoutInvalidatedEventId) >= 1);
  CHECK(listener.Count(UIA_Selection_InvalidatedEventId) >= 1);
  CHECK(listener.PropertyCount(UIA_BoundingRectanglePropertyId) >= 1);
  CHECK(listener.PropertyCount(UIA_IsOffscreenPropertyId) >= 1);
  CHECK(listener.StructureCount() >= 1);

  BOOL offscreen = TRUE;
  CHECK(SUCCEEDED(root->get_CurrentIsOffscreen(&offscreen)) && !offscreen);
  RECT root_bounds{};
  CHECK(SUCCEEDED(root->get_CurrentBoundingRectangle(&root_bounds)) &&
        root_bounds.right > root_bounds.left &&
        root_bounds.bottom > root_bounds.top);

  ComPtr<IUIAutomationCondition> true_condition;
  CHECK(SUCCEEDED(
      automation->CreateTrueCondition(true_condition.GetAddressOf())));
  ComPtr<IUIAutomationElementArray> children;
  CHECK(SUCCEEDED(root->FindAll(TreeScope_Children, true_condition.Get(),
                                children.GetAddressOf())));
  int child_count = 0;
  CHECK(SUCCEEDED(children->get_Length(&child_count)) && child_count == 2);
  ComPtr<IUIAutomationElement> first;
  ComPtr<IUIAutomationElement> second;
  CHECK(SUCCEEDED(children->GetElement(0, first.GetAddressOf())));
  CHECK(SUCCEEDED(children->GetElement(1, second.GetAddressOf())));
  BSTR first_name = nullptr;
  BSTR second_name = nullptr;
  CHECK(SUCCEEDED(first->get_CurrentName(&first_name)) && first_name &&
        std::wstring_view(first_name, SysStringLen(first_name)) == L"\u4f60");
  CHECK(SUCCEEDED(second->get_CurrentName(&second_name)) && second_name &&
        std::wstring_view(second_name, SysStringLen(second_name)) == L"\u5c3c");
  SysFreeString(first_name);
  SysFreeString(second_name);

  VARIANT first_selected;
  VARIANT second_selected;
  VariantInit(&first_selected);
  VariantInit(&second_selected);
  CHECK(SUCCEEDED(first->GetCurrentPropertyValue(
      UIA_SelectionItemIsSelectedPropertyId, &first_selected)));
  CHECK(SUCCEEDED(second->GetCurrentPropertyValue(
      UIA_SelectionItemIsSelectedPropertyId, &second_selected)));
  CHECK(first_selected.vt == VT_BOOL && first_selected.boolVal == VARIANT_TRUE);
  CHECK(second_selected.vt == VT_BOOL &&
        second_selected.boolVal == VARIANT_FALSE);
  VariantClear(&first_selected);
  VariantClear(&second_selected);

  ComPtr<IUIAutomationSelectionPattern> selection;
  CHECK(SUCCEEDED(root->GetCurrentPatternAs(
      UIA_SelectionPatternId, IID_PPV_ARGS(selection.GetAddressOf()))));
  ComPtr<IUIAutomationElementArray> selected_items;
  CHECK(SUCCEEDED(
      selection->GetCurrentSelection(selected_items.GetAddressOf())));
  int selected_count = 0;
  CHECK(SUCCEEDED(selected_items->get_Length(&selected_count)) &&
        selected_count == 1);

  RECT first_bounds{};
  CHECK(SUCCEEDED(first->get_CurrentBoundingRectangle(&first_bounds)) &&
        first_bounds.left >= root_bounds.left &&
        first_bounds.top >= root_bounds.top &&
        first_bounds.right <= root_bounds.right &&
        first_bounds.bottom <= root_bounds.bottom);

  auto selected = std::make_shared<RuntimeSnapshot>(*shown);
  selected->revision = 2;
  ++selected->correlation.sequence;
  selected->composition.highlighted_index = 1;
  const LONG selection_invalidated_before =
      listener.Count(UIA_Selection_InvalidatedEventId);
  window.Publish(selected);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.selection_only >= 1;
  }));
  CHECK(listener.Wait(UIA_SelectionItem_ElementSelectedEventId));
  Sleep(25);
  CHECK(listener.Count(UIA_SelectionItem_ElementSelectedEventId) == 1);
  CHECK(listener.Count(UIA_Selection_InvalidatedEventId) >
        selection_invalidated_before);
  CHECK(listener.PropertyCount(UIA_SelectionItemIsSelectedPropertyId) >= 2);
  VariantInit(&second_selected);
  CHECK(SUCCEEDED(second->GetCurrentPropertyValue(
      UIA_SelectionItemIsSelectedPropertyId, &second_selected)) &&
        second_selected.vt == VT_BOOL &&
        second_selected.boolVal == VARIANT_TRUE);
  VariantClear(&second_selected);

  auto moved = std::make_shared<RuntimeSnapshot>(*selected);
  moved->revision = 3;
  ++moved->correlation.sequence;
  ++moved->ui_sequence;
  moved->ui_state.caret = {700, 450, 702, 470};
  const LONG bounds_before =
      listener.PropertyCount(UIA_BoundingRectanglePropertyId);
  const LONG layout_before = listener.Count(UIA_LayoutInvalidatedEventId);
  window.Publish(moved);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.anchor_only >= 1;
  }));
  RECT moved_root{};
  RECT moved_second{};
  CHECK(SUCCEEDED(root->get_CurrentBoundingRectangle(&moved_root)) &&
        SUCCEEDED(second->get_CurrentBoundingRectangle(&moved_second)));
  CHECK(moved_root.left != root_bounds.left || moved_root.top != root_bounds.top);
  CHECK(moved_second.left >= moved_root.left &&
        moved_second.top >= moved_root.top &&
        moved_second.right <= moved_root.right &&
        moved_second.bottom <= moved_root.bottom);
  Sleep(25);
  CHECK(listener.PropertyCount(UIA_BoundingRectanglePropertyId) >
        bounds_before);
  CHECK(listener.Count(UIA_LayoutInvalidatedEventId) > layout_before);

  auto hidden = std::make_shared<RuntimeSnapshot>(*moved);
  hidden->revision = 4;
  ++hidden->correlation.sequence;
  hidden->ui_state.show_allowed = false;
  const LONG offscreen_before =
      listener.PropertyCount(UIA_IsOffscreenPropertyId);
  const LONG structure_before = listener.StructureCount();
  window.Publish(hidden);
  CHECK(WaitForVisibility(false));
  CHECK(listener.Wait(UIA_MenuClosedEventId));
  auto still_hidden = std::make_shared<RuntimeSnapshot>(*hidden);
  still_hidden->revision = 5;
  ++still_hidden->correlation.sequence;
  window.Publish(still_hidden);
  CHECK(window.Prewarm());
  Sleep(25);
  CHECK(listener.Count(UIA_MenuClosedEventId) == 1);
  CHECK(listener.PropertyCount(UIA_IsOffscreenPropertyId) > offscreen_before);
  CHECK(listener.StructureCount() > structure_before);
  CHECK(SUCCEEDED(root->get_CurrentIsOffscreen(&offscreen)) && offscreen);

  CHECK(listener.Finish());
  window.Stop();
  return true;
}

bool PrewarmCompletesBeforeReturn() {
  CandidateWindow window;
  const auto cold_started = std::chrono::steady_clock::now();
  CHECK(window.Start());
  CHECK(window.Prewarm());
  const auto renderer_ready = std::chrono::steady_clock::now();
  auto visible = VisibleSnapshot();
  const auto published = std::chrono::steady_clock::now();
  window.Publish(visible);
  CHECK(WaitForVisibility(true));
  const double cold_ready_ms =
      std::chrono::duration<double, std::milli>(renderer_ready - cold_started)
          .count();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - published)
          .count();
  std::printf("candidate_prewarm_boundary: cold_worker_resource_ready_ms=%.3f "
              "visible_publish_to_frame_ms=%.3f target_dpi=192\n",
              cold_ready_ms, elapsed_ms);
  window.Stop();
  CHECK(elapsed_ms <= 50.0);
  return true;
}

bool HiddenHighDpiStateDoesNotDelayFirstVisible() {
  CandidateWindow window;
  CHECK(window.Start());
  CHECK(window.Prewarm());
  auto hidden = VisibleSnapshot(10);
  hidden->revision = 1;
  hidden->ui_state.show_allowed = false;
  window.Publish(hidden);
  auto visible = VisibleSnapshot(11);
  visible->revision = 2;
  const auto published = std::chrono::steady_clock::now();
  window.Publish(visible);
  CHECK(WaitForVisibility(true));
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - published)
          .count();
  std::printf("candidate_hidden_high_dpi: visible_publish_to_frame_ms=%.3f "
              "target_dpi=192\n",
              elapsed_ms);
  window.Stop();
  CHECK(elapsed_ms <= 50.0);
  return true;
}

bool InlineHostPreeditStillShowsPanelHeader() {
  CandidateWindow window;
  CHECK(window.Start());
  CHECK(window.Prewarm());
  auto shown = VisibleSnapshot(30);
  shown->revision = 1;
  shown->composition.state_flags |= kHostInlinePreedit;
  shown->composition.preedit_cursor_pos = 2;
  window.Publish(shown);
  WindowProbe probe;
  CHECK(WaitForVisibility(true, &probe));
  const LONG shown_height = probe.rect.bottom - probe.rect.top;
  const uint64_t full_before = window.counters().full;

  FamoSkin hidden_skin = FamoSkinDefault();
  hidden_skin.show_preedit = 0;
  auto hidden_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::make_shared<const CandidateStylePresentation>(
                               CandidateStylePresentation{hidden_skin,
                                                          hidden_skin})});
  window.ActivateStyle(hidden_style);
  CHECK(WaitForCounters(window, [&](CandidateWindow::Counters counters) {
    return counters.full > full_before;
  }));
  probe = Probe();
  const LONG hidden_height = probe.rect.bottom - probe.rect.top;
  window.Stop();
  CHECK(shown_height > hidden_height);
  return true;
}

bool HealthyWindowAndHideRules() {
  CandidateWindow window;
  CHECK(window.Start());
  const HWND foreground = GetForegroundWindow();
  window.Publish(VisibleSnapshot());
  WindowProbe probe;
  CHECK(WaitForVisibility(true, &probe));
  CHECK((probe.ex_style & WS_EX_NOACTIVATE) != 0);
  CHECK((probe.ex_style & WS_EX_TOOLWINDOW) != 0);
  CHECK((probe.ex_style & WS_EX_LAYERED) != 0);
  CHECK((probe.ex_style & WS_EX_TOPMOST) != 0);
  CHECK(probe.cursor ==
        LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
  CHECK(GetForegroundWindow() == foreground);

  struct Placement {
    UiRect caret;
    UiRect work_area;
    uint32_t dpi;
  };
  constexpr Placement placements[] = {
      {{-12, 1020, -10, 1040}, {-1920, 0, 0, 1080}, 96},
      {{1910, 1050, 1912, 1070}, {0, 0, 1920, 1080}, 192},
      {{4400, 1180, 4402, 1200}, {1920, -200, 4480, 1240}, 144}};
  uint64_t placement_sequence = 4;
  for (const Placement &placement : placements) {
    auto moved = VisibleSnapshot(placement_sequence++);
    moved->ui_state.caret = placement.caret;
    moved->ui_state.work_area = placement.work_area;
    moved->ui_state.dpi = placement.dpi;
    window.Publish(moved);
    for (int attempt = 0; attempt < 100; ++attempt) {
      probe = Probe();
      if (probe.window && probe.visible &&
          probe.rect.left >= placement.work_area.left - 64 &&
          probe.rect.top >= placement.work_area.top - 64 &&
          probe.rect.right <= placement.work_area.right + 64 &&
          probe.rect.bottom <= placement.work_area.bottom + 64) {
        break;
      }
      Sleep(10);
    }
    CHECK(probe.window && probe.visible);
    if (probe.rect.left < placement.work_area.left - 64 ||
        probe.rect.top < placement.work_area.top - 64 ||
        probe.rect.right > placement.work_area.right + 64 ||
        probe.rect.bottom > placement.work_area.bottom + 64) {
      std::fprintf(stderr,
                   "placement outside work area: window=(%ld,%ld,%ld,%ld) "
                   "work=(%ld,%ld,%ld,%ld) dpi=%u\n",
                   probe.rect.left, probe.rect.top, probe.rect.right,
                   probe.rect.bottom,
                   static_cast<long>(placement.work_area.left),
                   static_cast<long>(placement.work_area.top),
                   static_cast<long>(placement.work_area.right),
                   static_cast<long>(placement.work_area.bottom),
                   placement.dpi);
    }
    CHECK(probe.rect.left >= placement.work_area.left - 64);
    CHECK(probe.rect.top >= placement.work_area.top - 64);
    CHECK(probe.rect.right <= placement.work_area.right + 64);
    CHECK(probe.rect.bottom <= placement.work_area.bottom + 64);
    CHECK(GetForegroundWindow() == foreground);
  }

  auto newest = VisibleSnapshot(placement_sequence++);
  newest->revision = 100;
  newest->ui_state.caret = {-1500, 300, -1498, 320};
  window.Publish(newest);
  CHECK(WaitForVisibility(true, &probe));
  for (int attempt = 0; attempt < 100 && probe.rect.left > -1000; ++attempt) {
    Sleep(1);
    probe = Probe();
  }
  CHECK(probe.rect.left < -1000);
  auto stale = VisibleSnapshot(placement_sequence++);
  stale->revision = 99;
  stale->ui_state.caret = {1500, 300, 1502, 320};
  window.Publish(stale);
  Sleep(25);
  probe = Probe();
  CHECK(probe.rect.left < -1000);

  FamoSkin wide_skin = FamoSkinDefault();
  wide_skin.min_width = 600;
  auto independent_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::make_shared<const CandidateStylePresentation>(
                               CandidateStylePresentation{wide_skin,
                                                          wide_skin})});
  window.ActivateStyle(independent_style);
  auto snapshot_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{
          0, std::make_shared<const CandidateStylePresentation>(
                 CandidateStylePresentation{FamoSkinDefault(),
                                            FamoSkinDefault()})});
  auto styled = VisibleSnapshot(placement_sequence++);
  styled->revision = 200;
  styled->style = snapshot_style;
  window.Publish(styled);
  for (int attempt = 0; attempt < 100; ++attempt) {
    probe = Probe();
    if (probe.window && probe.visible && probe.rect.right - probe.rect.left < 800)
      break;
    Sleep(1);
  }
  CHECK(probe.rect.right - probe.rect.left < 800);

  auto denied = VisibleSnapshot(placement_sequence++);
  denied->ui_state.show_allowed = false;
  window.Publish(denied);
  CHECK(WaitForVisibility(false));

  auto unavailable = VisibleSnapshot(placement_sequence++);
  unavailable->ui_state.layout_available = false;
  window.Publish(unavailable);
  CHECK(WaitForVisibility(false));

  auto unfocused = VisibleSnapshot(placement_sequence++);
  unfocused->ui_state.focused = false;
  window.Publish(unfocused);
  CHECK(WaitForVisibility(false));

  auto unapplied = VisibleSnapshot(placement_sequence++);
  unapplied->ui_sequence = unapplied->composition_sequence;
  window.Publish(unapplied);
  CHECK(WaitForVisibility(false));
  window.Stop();
  return true;
}

bool FirstVisibleBudgetAfterPrewarm() {
  std::vector<double> prewarm_ms;
  std::vector<double> samples_ms;
  for (uint64_t sample = 0; sample < 100; ++sample) {
    CandidateWindow window;
    CHECK(window.Start());
    const auto prewarm_started = std::chrono::steady_clock::now();
    CHECK(window.Prewarm());
    auto hidden = VisibleSnapshot(sample * 2 + 10);
    hidden->revision = sample * 2 + 1;
    hidden->ui_state.show_allowed = false;
    window.Publish(hidden);
    prewarm_ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - prewarm_started)
                             .count());
    const auto published = std::chrono::steady_clock::now();
    auto visible = VisibleSnapshot(sample * 2 + 11);
    visible->revision = hidden->revision + 1;
    window.Publish(visible);
    CHECK(WaitForVisibility(true));
    samples_ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - published)
                             .count());
    window.Stop();
  }
  std::sort(prewarm_ms.begin(), prewarm_ms.end());
  std::sort(samples_ms.begin(), samples_ms.end());
  const double p99 = samples_ms[(samples_ms.size() * 99 - 1) / 100];
  const double prewarm_p99 =
      prewarm_ms[(prewarm_ms.size() * 99 - 1) / 100];
  std::printf("candidate_first_visible: samples=%zu target_dpi=192 "
              "resource_prewarm_p99_ms=%.3f "
              "visible_p99_ms=%.3f visible_max_ms=%.3f\n",
              samples_ms.size(), prewarm_p99, p99, samples_ms.back());
  CHECK(p99 <= 50.0);
  return true;
}

bool FastPathsAndDeviceRecoveryAreObservable() {
  CandidateWindow window;
  CHECK(window.Start());
  auto initial = VisibleSnapshot(30);
  initial->revision = 1;
  window.Publish(initial);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.full >= 1;
  }));

  auto duplicate = VisibleSnapshot(31);
  duplicate->revision = 2;
  duplicate->ui_state.selection_capability =
      initial->ui_state.selection_capability;
  window.Publish(duplicate);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.duplicate >= 1;
  }));

  auto moved = VisibleSnapshot(32);
  moved->revision = 3;
  moved->ui_state.selection_capability =
      duplicate->ui_state.selection_capability;
  moved->ui_state.caret = {700, 450, 702, 470};
  window.Publish(moved);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.anchor_only >= 1;
  }));
  WindowProbe moved_probe = Probe();
  CHECK(moved_probe.visible);
  CHECK(moved_probe.rect.left != initial_probe.rect.left ||
        moved_probe.rect.top != initial_probe.rect.top);

  auto selected = VisibleSnapshot(33);
  selected->revision = 4;
  selected->ui_state = moved->ui_state;
  selected->composition.highlighted_index = 1;
  window.Publish(selected);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.selection_only >= 1;
  }));
  CHECK(PostMessageW(moved_probe.window, WM_THEMECHANGED, 0, 0) != FALSE);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.full >= 2;
  }));
  const CandidateWindow::Counters counters = window.counters();
  std::printf("candidate_fast_paths: full=%llu duplicate=%llu anchor=%llu "
              "selection=%llu\n",
              static_cast<unsigned long long>(counters.full),
              static_cast<unsigned long long>(counters.duplicate),
              static_cast<unsigned long long>(counters.anchor_only),
              static_cast<unsigned long long>(counters.selection_only));
  window.Stop();

  CandidateWindow recovering(CandidateWindow::Fault::DeviceLossOnce);
  CHECK(recovering.Start());
  auto recover_snapshot = VisibleSnapshot(40);
  recover_snapshot->revision = 1;
  recovering.Publish(recover_snapshot);
  CHECK(WaitForVisibility(true));
  CHECK(WaitForCounters(
      recovering, [](const CandidateWindow::Counters &value) {
        return value.device_recovery == 1 && value.full >= 1;
      }));
  recovering.Stop();
  return true;
}

bool ModeIndicatorRequiresFreshCaretAndDeduplicates() {
  CandidateWindow window;
  CHECK(window.Start());
  CHECK(window.Prewarm());
  auto stale = VisibleSnapshot(60);
  stale->revision = 1;
  stale->composition.candidates.clear();
  stale->ui_state.show_allowed = false;
  stale->composition.status_flags = FAMO_STATUS_ASCII_MODE;
  stale->mode_switch_sequence = stale->ui_sequence;
  window.Publish(stale);
  Sleep(25);
  CHECK(window.counters().mode_indicator == 0);

  auto fresh = std::make_shared<RuntimeSnapshot>(*stale);
  fresh->revision = 2;
  fresh->correlation.sequence++;
  fresh->ui_sequence++;
  window.Publish(fresh);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.mode_indicator == 1;
  }));

  auto duplicate = std::make_shared<RuntimeSnapshot>(*fresh);
  duplicate->revision = 3;
  duplicate->correlation.sequence++;
  duplicate->ui_sequence++;
  window.Publish(duplicate);
  Sleep(25);
  CHECK(window.counters().mode_indicator == 1);

  auto chinese = std::make_shared<RuntimeSnapshot>(*duplicate);
  chinese->revision = 4;
  chinese->correlation.sequence++;
  chinese->composition_sequence = chinese->correlation.sequence;
  chinese->mode_switch_sequence = chinese->composition_sequence;
  chinese->ui_sequence = chinese->composition_sequence + 1;
  chinese->composition.status_flags = 0;
  window.Publish(chinese);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.mode_indicator == 2;
  }));
  window.Stop();
  return true;
}

bool PaintFailureHidesWithoutBlockingEngine() {
  CandidateWindow window(CandidateWindow::Fault::PaintAfterVisible);
  CHECK(window.Start());
  window.Publish(VisibleSnapshot());
  CHECK(WaitForVisibility(true));
  auto changed = VisibleSnapshot(4);
  changed->composition.highlighted_index = 1;
  window.Publish(changed);
  CHECK(WaitForVisibility(false));

  RuntimeService service;
  service.SetSnapshotSink(&window);
  std::string error;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {21, 22, 23, 0, 0, 0};
  CHECK(service.Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {21, 22, 23, 24, 25, 1};
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::Ok);
  Frame key;
  key.command = Command::ProcessKey;
  key.correlation = {21, 22, 23, 24, 25, 2};
  CHECK(EncodeKeyEvent({'N', 0, 0, 1, 1}, &key.payload));
  const auto key_started = std::chrono::steady_clock::now();
  CHECK(service.Dispatch(key).status == Status::Ok);
  CHECK(std::chrono::steady_clock::now() - key_started <
        std::chrono::milliseconds(50));
  service.SetSnapshotSink(nullptr);
  service.Stop();
  window.Stop();
  return true;
}

bool FaultsNeverBlockPublisher() {
  constexpr CandidateWindow::Fault faults[] = {
      CandidateWindow::Fault::Create, CandidateWindow::Fault::Layout,
      CandidateWindow::Fault::Paint, CandidateWindow::Fault::Submit,
      CandidateWindow::Fault::Hang};
  for (CandidateWindow::Fault fault : faults) {
    CandidateWindow window(fault);
    CHECK(window.Start());
    const auto publish_started = std::chrono::steady_clock::now();
    window.Publish(VisibleSnapshot());
    CHECK(std::chrono::steady_clock::now() - publish_started <
          std::chrono::milliseconds(10));
    Sleep(25);
    const auto stop_started = std::chrono::steady_clock::now();
    window.Stop();
    CHECK(std::chrono::steady_clock::now() - stop_started <
          std::chrono::milliseconds(400));
  }
  return true;
}

bool HangingUiDoesNotDelayEngine() {
  CandidateWindow window(CandidateWindow::Fault::Hang);
  CHECK(window.Start());
  RuntimeService service;
  service.SetSnapshotSink(&window);
  std::string error;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {11, 12, 13, 0, 0, 0};
  CHECK(service.Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {11, 12, 13, 14, 15, 1};
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::Ok);
  Frame key;
  key.command = Command::ProcessKey;
  key.correlation = {11, 12, 13, 14, 15, 2};
  CHECK(EncodeKeyEvent({'N', 0, 0, 1, 1}, &key.payload));
  const auto started = std::chrono::steady_clock::now();
  CHECK(service.Dispatch(key).status == Status::Ok);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(50));
  service.Stop();
  service.SetSnapshotSink(nullptr);
  window.Stop();
  return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc > 1 && std::wstring_view(argv[1]) == L"--fake-preview-target")
    return RunFakePreviewTarget(argc, argv);
  if (argc > 1 && std::wstring_view(argv[1]) == L"--uia-listener")
    return RunAutomationListener(argc, argv);
  if (argc > 1 && std::wstring_view(argv[1]) == L"--light-dismiss-events") {
    if (!CandidateWindowReportsShowEvent() ||
        !CandidateWindowReportsMoveEvent() ||
        !CandidateWindowDoesNotReportNoOpMove() ||
        !CandidateWindowReportsSizeChangeEvent() ||
        !CandidateWindowReportsHideEvent() ||
        !CandidateWindowNeverReportsUnshownWindow() ||
        !CandidateWindowDoesNotRepeatLightDismissEvents() ||
        !CandidateWindowReportsHideWhenStopped()) {
      return 1;
    }
    std::printf("candidate_window_light_dismiss: OK\n");
    return 0;
  }
  if (!PreviewRowsMapToAbsoluteCandidateIndexes() ||
      !CandidateWindowUsesExplicitInProcessOwner() ||
      !CandidateWindowFollowsOwnerLifecycle() ||
      !CandidateWindowReleasesRegisteredClasses() ||
      !CandidateWindowTakesAppearanceFromPublishedStyle() ||
      !PreviewRoutingSupportsInProcessTarget() ||
      !PreviewRoutingBindsExactOwnerAndSourceWindow() ||
      !ScrollTransitionIsBoundedAndOptional() ||
      !CandidateWindowReportsShowEvent() ||
      !CandidateWindowReportsMoveEvent() ||
      !CandidateWindowRelayoutsWhenWorkAreaShrinks() ||
      !CandidateWindowDoesNotReportNoOpMove() ||
      !CandidateWindowReportsSizeChangeEvent() ||
      !CandidateWindowReportsHideEvent() ||
      !CandidateWindowNeverReportsUnshownWindow() ||
      !CandidateWindowDoesNotRepeatLightDismissEvents() ||
      !CandidateWindowReportsHideWhenStopped() ||
      !CandidateWindowExposesAccessibleTreeAndEvents() ||
      !PrewarmCompletesBeforeReturn() ||
      !HiddenHighDpiStateDoesNotDelayFirstVisible() ||
      !InlineHostPreeditStillShowsPanelHeader() ||
      !HealthyWindowAndHideRules() || !FirstVisibleBudgetAfterPrewarm() ||
      !FastPathsAndDeviceRecoveryAreObservable() ||
      !ModeIndicatorRequiresFreshCaretAndDeduplicates() ||
      !PaintFailureHidesWithoutBlockingEngine() ||
      !FaultsNeverBlockPublisher() || !HangingUiDoesNotDelayEngine())
    return 1;
  std::printf("candidate_window_selfcheck: OK\n");
  return 0;
}
