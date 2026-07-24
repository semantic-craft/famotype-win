#include "text_service.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <new>
#include <utility>

#include "famo_guids.h"
#include "module_state.h"

namespace famo::tsf {

namespace {

std::atomic<uint64_t> g_activation_generation{0};
constexpr UINT kRecoveryMessage = WM_APP + 0x46;
constexpr int kSessionOpenAttempts = 3;
constexpr std::chrono::milliseconds kSessionRetryDelay{20};

ComPtr<ITfContext> TopContext(ITfDocumentMgr *document) {
  ComPtr<ITfContext> context;
  if (document)
    document->GetTop(context.put());
  return context;
}

bool TimingEnabled() {
  wchar_t value[8]{};
  const DWORD length = GetEnvironmentVariableW(
      L"FAMO_LOCAL_TIMING", value, static_cast<DWORD>(std::size(value)));
  return length > 0 && length < std::size(value) &&
         (wcscmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0 ||
          _wcsicmp(value, L"yes") == 0);
}

} // namespace

TextService::TextService()
    : TextService(kRuntimeEndpointSuffix, L"FamoRuntime.exe", "") {}

TextService::TextService(std::wstring runtime_endpoint_suffix)
    : TextService(std::move(runtime_endpoint_suffix), L"FamoTestRuntime.exe",
                  "test") {}

TextService::TextService(std::wstring runtime_endpoint_suffix,
                         std::wstring runtime_executable_name,
                         std::string schema_id)
    : runtime_endpoint_suffix_(std::move(runtime_endpoint_suffix)),
      runtime_executable_name_(std::move(runtime_executable_name)),
      schema_id_(std::move(schema_id)) {
  AddModuleObject();
}

TextService::~TextService() {
  if (thread_manager_ && OnActivationThread())
    Deactivate();
  if (session_worker_.joinable())
    StopSessionWorker();
  runtime_port_.Stop();
  RemoveModuleObject();
}

HRESULT TextService::QueryInterface(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_ITfTextInputProcessor ||
      iid == IID_ITfTextInputProcessorEx)
    *object = static_cast<ITfTextInputProcessorEx *>(this);
  else if (iid == IID_ITfKeyEventSink)
    *object = static_cast<ITfKeyEventSink *>(this);
  else if (iid == IID_ITfThreadMgrEventSink)
    *object = static_cast<ITfThreadMgrEventSink *>(this);
  else if (iid == IID_ITfCompositionSink)
    *object = static_cast<ITfCompositionSink *>(this);
  else if (iid == IID_ITfTextLayoutSink)
    *object = static_cast<ITfTextLayoutSink *>(this);
  if (!*object)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

ULONG TextService::AddRef() { return ++references_; }

ULONG TextService::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0)
    delete this;
  return remaining;
}

HRESULT TextService::Activate(ITfThreadMgr *thread_manager,
                              TfClientId client_id) {
  return ActivateEx(thread_manager, client_id, 0);
}

HRESULT TextService::ActivateEx(ITfThreadMgr *thread_manager,
                                TfClientId client_id, DWORD) {
  return ActivateCore(thread_manager, client_id, true);
}

HRESULT TextService::ActivateForTest(ITfThreadMgr *thread_manager,
                                     TfClientId client_id) {
  return ActivateCore(thread_manager, client_id, false);
}

HRESULT TextService::ActivateCore(ITfThreadMgr *thread_manager,
                                  TfClientId client_id,
                                  bool advise_key_sink) {
  if (!thread_manager || thread_manager_)
    return E_INVALIDARG;
  activation_thread_ = GetCurrentThreadId();
  client_id_ = client_id;
  thread_manager_ = ComPtr<ITfThreadMgr>(thread_manager);
  runtime_client_id_ =
      (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
      static_cast<uint64_t>(GetTickCount64()) ^
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  if (runtime_client_id_ == 0)
    runtime_client_id_ = 1;
  activation_generation_ = ++g_activation_generation;
  timing_enabled_ = TimingEnabled();

  HRESULT result = thread_manager_->QueryInterface(
      IID_ITfKeystrokeMgr,
      reinterpret_cast<void **>(keystroke_manager_.put()));
  if (FAILED(result)) {
    Deactivate();
    return result;
  }
  if (advise_key_sink) {
    result = keystroke_manager_->AdviseKeyEventSink(
        client_id_, static_cast<ITfKeyEventSink *>(this), TRUE);
    if (FAILED(result)) {
      Deactivate();
      return result;
    }
    key_sink_advised_ = true;
  }

  ComPtr<ITfSource> source;
  result = thread_manager_->QueryInterface(
      IID_ITfSource, reinterpret_cast<void **>(source.put()));
  if (FAILED(result)) {
    Deactivate();
    return result;
  }
  result = source->AdviseSink(IID_ITfThreadMgrEventSink,
                              static_cast<ITfThreadMgrEventSink *>(this),
                              &thread_sink_cookie_);
  if (FAILED(result)) {
    Deactivate();
    return result;
  }
  result = thread_manager_->QueryInterface(
      IID_ITfUIElementMgr,
      reinterpret_cast<void **>(ui_manager_.put()));
  if (FAILED(result)) {
    Deactivate();
    return result;
  }
  if (!StartRecoveryWindow() || !StartSessionWorker()) {
    Deactivate();
    return E_OUTOFMEMORY;
  }

  ComPtr<ITfDocumentMgr> focus;
  if (SUCCEEDED(thread_manager_->GetFocus(focus.put()))) {
    ComPtr<ITfContext> context = TopContext(focus.get());
    if (context)
      EnsureContext(context.get(), SessionWarmupReason::Activation);
  }
  return S_OK;
}

HRESULT TextService::Deactivate() {
  if (!thread_manager_)
    return S_OK;
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;

  StopSessionWorker();
  StopRecoveryWindow();
  for (auto &entry : contexts_)
    CloseEntry(entry.get());
  contexts_.clear();

  if (keystroke_manager_ && key_sink_advised_)
    keystroke_manager_->UnadviseKeyEventSink(client_id_);
  key_sink_advised_ = false;
  if (thread_sink_cookie_ != TF_INVALID_COOKIE) {
    ComPtr<ITfSource> source;
    if (SUCCEEDED(thread_manager_->QueryInterface(
            IID_ITfSource, reinterpret_cast<void **>(source.put()))))
      source->UnadviseSink(thread_sink_cookie_);
    thread_sink_cookie_ = TF_INVALID_COOKIE;
  }
  runtime_port_.Stop();
  ui_manager_.reset();
  keystroke_manager_.reset();
  thread_manager_.reset();
  client_id_ = TF_CLIENTID_NULL;
  activation_thread_ = 0;
  return S_OK;
}

bool TextService::OnActivationThread() const {
  return activation_thread_ != 0 && activation_thread_ == GetCurrentThreadId();
}

bool TextService::ConnectRuntime(const runtime::Correlation &identity) {
  if (runtime_port_.state() == runtime::ChannelState::Ready &&
      runtime_port_.connection_generation() ==
          identity.connection_generation) {
    return true;
  }
  runtime_port_.Stop();
  runtime::PipeEndpoint endpoint;
  std::string error;
  if (!runtime::BuildCurrentPipeEndpoint(runtime_endpoint_suffix_, &endpoint,
                                         &error))
    return false;
  const std::wstring expected =
      ModuleDirectory() + L"\\" + runtime_executable_name_;
  std::chrono::milliseconds deadline{500};
  if (runtime_executable_name_ == L"FamoRuntime.exe") {
    if (!WaitNamedPipeW(endpoint.name.c_str(), 0) &&
        GetLastError() == ERROR_FILE_NOT_FOUND) {
      std::wstring command = L"\"" + expected + L"\"";
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      if (CreateProcessW(expected.c_str(), command.data(), nullptr, nullptr,
                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
      }
    }
    deadline = std::chrono::seconds(2);
  }
  runtime::Correlation connection = identity;
  connection.session_id = 0;
  connection.session_generation = 0;
  connection.sequence = 0;
  return runtime_port_.Connect(endpoint, expected, connection, deadline, &error,
                               &session_worker_stop_);
}

HRESULT TextService::EnsureContext(ITfContext *context,
                                   SessionWarmupReason reason) {
  if (!context || !OnActivationThread())
    return E_INVALIDARG;
  ApplySessionResult();
  ContextEntry *entry = FindContext(context);
  if (entry && entry->recovery_cleanup_required) {
    ProcessRecoveryWork();
    if (entry->recovery_cleanup_required)
      return S_FALSE;
  }
  if (entry && entry->state.phase() == ContextPhase::Ready &&
      entry->state.session_identity().connection_generation ==
          connection_generation_)
    return S_OK;

  if (!entry) {
    auto owned = std::make_unique<ContextEntry>();
    owned->context = ComPtr<ITfContext>(context);
    context->GetDocumentMgr(owned->document.put());
    owned->candidates.reset(
        new (std::nothrow) CandidateUiElement(ui_manager_.get(),
                                             owned->document.get()));
    if (!owned->candidates)
      return E_OUTOFMEMORY;
    ComPtr<ITfSource> context_source;
    if (FAILED(context->QueryInterface(
            IID_ITfSource,
            reinterpret_cast<void **>(context_source.put()))) ||
        FAILED(context_source->AdviseSink(
            IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink *>(this),
            &owned->layout_sink_cookie))) {
      return S_FALSE;
    }
    contexts_.push_back(std::move(owned));
    entry = contexts_.back().get();
  } else if (entry->candidates) {
    entry->candidates->End();
  }
  SetFocused(entry, true);
  if (!entry->session_pending)
    ScheduleSession(entry, reason);
  return S_FALSE;
}

bool TextService::StartSessionWorker() {
  session_worker_stop_.store(false);
  session_disconnect_requested_.store(false);
  session_request_.store(nullptr);
  session_result_.store(nullptr);
  desired_session_.store(nullptr);
  try {
    session_worker_ = std::thread(&TextService::SessionWorkerMain, this);
  } catch (...) {
    return false;
  }
  return true;
}

void TextService::StopSessionWorker() {
  session_worker_stop_.store(true);
  session_retry_wake_.notify_all();
  runtime_port_.CancelConnect();
  runtime_port_.CancelCall();
  session_worker_epoch_.fetch_add(1);
  session_worker_epoch_.notify_all();
  if (session_worker_.joinable())
    session_worker_.join();
  session_request_.store(nullptr);
  session_result_.store(nullptr);
  desired_session_.store(nullptr);
  session_disconnect_requested_.store(false);
  session_worker_stop_.store(false);
}

bool TextService::StartRecoveryWindow() {
  HWND window = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, ModuleHandle(), nullptr);
  if (!window)
    return false;
  SetWindowLongPtrW(window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(this));
  SetLastError(ERROR_SUCCESS);
  const LONG_PTR previous = SetWindowLongPtrW(
      window, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(&TextService::RecoveryWindowProc));
  if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
    return false;
  }
  recovery_window_ = window;
  recovery_previous_proc_ = reinterpret_cast<WNDPROC>(previous);
  recovery_message_posted_ = false;
  return true;
}

void TextService::StopRecoveryWindow() {
  recovery_message_posted_ = false;
  if (recovery_window_)
    DestroyWindow(recovery_window_);
  recovery_window_ = nullptr;
  recovery_previous_proc_ = nullptr;
}

void TextService::PostRecoveryWork() {
  if (!recovery_window_ || recovery_message_posted_)
    return;
  recovery_message_posted_ =
      PostMessageW(recovery_window_, kRecoveryMessage, 0, 0) != FALSE;
}

void TextService::ProcessRecoveryWork() {
  recovery_message_posted_ = false;
  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    if (!entry->recovery_cleanup_required || entry->recovery_preedit.empty())
      continue;
    const HRESULT recovered = entry->composition.Recover(
        entry->context.get(), client_id_, entry->recovery_preedit,
        static_cast<ITfCompositionSink *>(this));
    if (SUCCEEDED(recovered)) {
      entry->recovery_cleanup_required = false;
      entry->recovery_preedit.clear();
    }
  }
  ApplySessionResult();
}

LRESULT CALLBACK TextService::RecoveryWindowProc(HWND window, UINT message,
                                                 WPARAM wparam,
                                                 LPARAM lparam) {
  auto *service = reinterpret_cast<TextService *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!service)
    return DefWindowProcW(window, message, wparam, lparam);
  WNDPROC previous = service->recovery_previous_proc_;
  if (message == kRecoveryMessage) {
    service->ProcessRecoveryWork();
    return 0;
  }
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    service->recovery_window_ = nullptr;
    service->recovery_previous_proc_ = nullptr;
  }
  return previous ? CallWindowProcW(previous, window, message, wparam, lparam)
                  : DefWindowProcW(window, message, wparam, lparam);
}

void TextService::ScheduleSession(ContextEntry *entry,
                                  SessionWarmupReason reason) {
  if (!entry || entry->session_pending)
    return;
  uint64_t generation = runtime_port_.connection_generation();
  if (runtime_port_.state() != runtime::ChannelState::Ready) {
    generation = std::max(generation, connection_generation_ + 1);
  }
  connection_generation_ = generation;
  runtime::Correlation identity{
      runtime_client_id_, activation_generation_, connection_generation_,
      next_session_id_++, next_session_generation_++, 1};
  try {
    auto request = std::make_shared<const SessionWarmupRequest>(
        SessionWarmupRequest{identity, reason});
    {
      std::lock_guard lock(session_publication_mutex_);
      const std::shared_ptr<const SessionWarmupResult> result =
          session_result_.load();
      for (auto &owned : contexts_) {
        ContextEntry *superseded = owned.get();
        if (superseded == entry || !superseded->session_pending)
          continue;
        if (result && result->identity == superseded->pending_session) {
          session_result_.store(nullptr);
          if (result->ready && !superseded->recovery_cleanup_required &&
              result->identity.activation_generation ==
                  activation_generation_ &&
              result->identity.connection_generation ==
                  connection_generation_ &&
              runtime_port_.state() == runtime::ChannelState::Ready) {
            superseded->state.Open(result->identity);
            superseded->first_key_pending = true;
          }
        }
        superseded->pending_session = {};
        superseded->session_pending = false;
      }
      desired_session_.store(
          std::shared_ptr<const runtime::Correlation>(request,
                                                      &request->identity));
      entry->pending_session = identity;
      entry->session_pending = true;
      session_request_.store(std::move(request));
    }
    session_worker_epoch_.fetch_add(1);
    session_worker_epoch_.notify_one();
    session_retry_wake_.notify_all();
  } catch (...) {
    entry->pending_session = {};
    entry->session_pending = false;
  }
}

void TextService::SessionWorkerMain() {
  uint64_t observed_epoch = session_worker_epoch_.load();
  while (!session_worker_stop_.load()) {
    const bool disconnect = session_disconnect_requested_.exchange(false);
    std::shared_ptr<const SessionWarmupRequest> request =
        session_request_.exchange(nullptr);
    if (!request && !disconnect) {
      session_worker_epoch_.wait(observed_epoch);
      observed_epoch = session_worker_epoch_.load();
      continue;
    }
    if (!request) {
      runtime_port_.Stop();
      continue;
    }

    const auto started = std::chrono::steady_clock::now();
    bool ready = ConnectRuntime(request->identity);
    if (ready && !session_worker_stop_.load()) {
      std::lock_guard lock(session_publication_mutex_);
      const std::shared_ptr<const runtime::Correlation> desired =
          desired_session_.load();
      ready = desired && *desired == request->identity;
    }
    bool opened = false;
    if (ready && !session_worker_stop_.load()) {
      std::string error;
      std::vector<uint8_t> payload;
      ready = runtime::EncodeOpenSession(schema_id_, &payload, &error);
      for (int attempt = 0;
           ready && attempt < kSessionOpenAttempts &&
           !session_worker_stop_.load();
           ++attempt) {
        const std::shared_ptr<const runtime::Correlation> desired =
            desired_session_.load();
        if (!desired || *desired != request->identity) {
          ready = false;
          break;
        }
        runtime::Frame open;
        open.command = runtime::Command::OpenSession;
        open.correlation = request->identity;
        open.payload = payload;
        runtime::CallResult result =
            runtime_port_.Call(std::move(open), runtime::kSessionOpenDeadline);
        ready = result.status == runtime::Status::Ok &&
                result.reply.correlation == request->identity &&
                result.reply.payload.empty();
        if (ready) {
          opened = true;
          break;
        }
        if (result.status != runtime::Status::Unavailable) {
          runtime_port_.Poison();
          break;
        }
        if (attempt + 1 >= kSessionOpenAttempts ||
            runtime_port_.state() != runtime::ChannelState::Ready) {
          break;
        }
        std::unique_lock retry_lock(session_retry_mutex_);
        session_retry_wake_.wait_for(retry_lock, kSessionRetryDelay, [&] {
          const std::shared_ptr<const runtime::Correlation> current =
              desired_session_.load();
          return session_worker_stop_.load() || !current ||
                 *current != request->identity;
        });
        const std::shared_ptr<const runtime::Correlation> current =
            desired_session_.load();
        ready = !session_worker_stop_.load() && current &&
                *current == request->identity;
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const char *operation = "activationWarmup";
    if (request->reason == SessionWarmupReason::Focus)
      operation = "focusWarmup";
    else if (request->reason == SessionWarmupReason::Recovery)
      operation = "recoveryWarmup";
    ReportTiming(operation, elapsed, request->identity,
                 ready ? runtime::Status::Ok
                       : runtime::Status::Unavailable);
    if (session_worker_stop_.load())
      break;
    std::shared_ptr<const SessionWarmupResult> outcome;
    try {
      outcome = std::make_shared<const SessionWarmupResult>(
          SessionWarmupResult{request->identity, ready});
    } catch (...) {
    }
    bool close_stale = false;
    {
      std::lock_guard lock(session_publication_mutex_);
      const std::shared_ptr<const runtime::Correlation> desired =
          desired_session_.load();
      if (outcome && desired && *desired == request->identity) {
        session_result_.store(std::move(outcome));
      } else {
        close_stale = opened;
      }
    }
    if (close_stale) {
      runtime::Frame close;
      close.command = runtime::Command::CloseSession;
      close.correlation = request->identity;
      ++close.correlation.sequence;
      if (runtime_port_.Call(std::move(close), runtime::kHardCallDeadline)
              .status != runtime::Status::Ok) {
        runtime_port_.Stop();
      }
    }
  }
}

void TextService::ReportTiming(const char *operation,
                               std::chrono::milliseconds elapsed,
                               const runtime::Correlation &identity,
                               runtime::Status status) const {
  if (!timing_enabled_ || !operation)
    return;
  char line[256]{};
  std::snprintf(
      line, std::size(line),
      "FamoTiming component=tsf operation=%s elapsedMs=%lld status=%u "
      "activationGeneration=%llu connectionGeneration=%llu "
      "sessionGeneration=%llu\n",
      operation, static_cast<long long>(elapsed.count()),
      static_cast<unsigned>(status),
      static_cast<unsigned long long>(identity.activation_generation),
      static_cast<unsigned long long>(identity.connection_generation),
      static_cast<unsigned long long>(identity.session_generation));
  OutputDebugStringA(line);
}

void TextService::ApplySessionResult() {
  std::unique_lock publication_lock(session_publication_mutex_,
                                    std::try_to_lock);
  if (!publication_lock)
    return;
  const std::shared_ptr<const SessionWarmupResult> result =
      session_result_.exchange(nullptr);
  if (!result || result->identity.activation_generation !=
                     activation_generation_)
    return;
  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    if (!entry->session_pending ||
        entry->pending_session != result->identity)
      continue;
    entry->session_pending = false;
    entry->pending_session = {};
    const std::shared_ptr<const runtime::Correlation> desired =
        desired_session_.load();
    if (desired && *desired == result->identity)
      desired_session_.store(nullptr);
    if (!result->ready || !entry->ui_state.focused ||
        result->identity.connection_generation != connection_generation_ ||
        runtime_port_.state() != runtime::ChannelState::Ready) {
      return;
    }
    entry->state.Open(result->identity);
    entry->first_key_pending = true;
    RefreshLayout(entry, nullptr);
    return;
  }
}

TextService::ContextEntry *TextService::FindContext(ITfContext *context) {
  const auto found = std::find_if(
      contexts_.begin(), contexts_.end(), [&](const auto &entry) {
        return SameComObject(entry->context.get(), context);
      });
  return found == contexts_.end() ? nullptr : found->get();
}

void TextService::CloseContext(ITfContext *context) {
  const auto found = std::find_if(
      contexts_.begin(), contexts_.end(), [&](const auto &entry) {
        return SameComObject(entry->context.get(), context);
      });
  if (found == contexts_.end())
    return;
  CloseEntry(found->get());
  contexts_.erase(found);
}

void TextService::CloseEntry(ContextEntry *entry) {
  if (!entry)
    return;
  ApplySessionResult();
  if (entry->candidates)
    entry->candidates->End();
  entry->composition.ResetBehaviorState();
  entry->recovery_cleanup_required = false;
  entry->recovery_preedit.clear();
  entry->ui_state.show_allowed = false;
  SetFocused(entry, false);
  if (entry->layout_sink_cookie != TF_INVALID_COOKIE) {
    ComPtr<ITfSource> source;
    if (SUCCEEDED(entry->context->QueryInterface(
            IID_ITfSource, reinterpret_cast<void **>(source.put()))))
      source->UnadviseSink(entry->layout_sink_cookie);
    entry->layout_sink_cookie = TF_INVALID_COOKIE;
  }
  entry->composition.End(entry->context.get(), client_id_);
  bool close_failed = false;
  if (runtime_port_.state() == runtime::ChannelState::Ready) {
    const auto correlation = entry->state.PlanClose();
    if (correlation) {
      runtime::Frame close;
      close.command = runtime::Command::CloseSession;
      close.correlation = *correlation;
      close_failed =
          runtime_port_.Call(std::move(close), runtime::kHardCallDeadline)
              .status != runtime::Status::Ok;
    }
  }
  entry->state.Close();
  if (close_failed && session_worker_.joinable())
    RecoverConnection();
}

HRESULT TextService::OnSetFocus(BOOL foreground) {
  if (!thread_manager_ || !OnActivationThread())
    return S_OK;
  ApplySessionResult();
  if (!foreground) {
    for (auto &entry : contexts_)
      SetFocused(entry.get(), false);
    return S_OK;
  }
  ComPtr<ITfDocumentMgr> focus;
  if (SUCCEEDED(thread_manager_->GetFocus(focus.put()))) {
    ComPtr<ITfContext> context = TopContext(focus.get());
    if (context) {
      EnsureContext(context.get());
      SetFocused(FindContext(context.get()), true);
    }
  }
  return S_OK;
}

HRESULT TextService::OnInitDocumentMgr(ITfDocumentMgr *) { return S_OK; }

HRESULT TextService::OnUninitDocumentMgr(ITfDocumentMgr *document) {
  for (size_t index = 0; index < contexts_.size();) {
    if (SameComObject(contexts_[index]->document.get(), document)) {
      CloseEntry(contexts_[index].get());
      contexts_.erase(contexts_.begin() + static_cast<ptrdiff_t>(index));
    } else {
      ++index;
    }
  }
  return S_OK;
}

HRESULT TextService::OnSetFocus(ITfDocumentMgr *focus,
                                ITfDocumentMgr *previous) {
  if (!OnActivationThread())
    return RPC_E_WRONG_THREAD;
  ApplySessionResult();
  ComPtr<ITfContext> previous_context = TopContext(previous);
  if (previous_context)
    SetFocused(FindContext(previous_context.get()), false);
  ComPtr<ITfContext> context = TopContext(focus);
  if (context) {
    EnsureContext(context.get());
    SetFocused(FindContext(context.get()), true);
  }
  return S_OK;
}

HRESULT TextService::OnPushContext(ITfContext *context) {
  return EnsureContext(context);
}

HRESULT TextService::OnPopContext(ITfContext *context) {
  ComPtr<ITfDocumentMgr> document;
  if (context)
    context->GetDocumentMgr(document.put());
  CloseContext(context);
  ComPtr<ITfContext> top = TopContext(document.get());
  if (!top || SameComObject(top.get(), context)) {
    const auto previous = std::find_if(
        contexts_.rbegin(), contexts_.rend(), [&](const auto &entry) {
          return SameComObject(entry->document.get(), document.get());
        });
    top = previous == contexts_.rend()
              ? ComPtr<ITfContext>()
              : ComPtr<ITfContext>((*previous)->context.get());
  }
  if (top)
    EnsureContext(top.get());
  return S_OK;
}

HRESULT TextService::OnCompositionTerminated(TfEditCookie,
                                             ITfComposition *composition) {
  for (auto &entry : contexts_) {
    if (entry->composition.CompositionTerminated(composition)) {
      entry->recovery_cleanup_required = false;
      entry->recovery_preedit.clear();
      if (entry->candidates)
        entry->candidates->End();
      entry->ui_state.show_allowed = false;
      PublishUiState(entry.get());
      entry->state.Fail();
      break;
    }
  }
  return S_OK;
}

HRESULT CreateTextServiceInstance(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  auto *service = new (std::nothrow) TextService();
  if (!service)
    return E_OUTOFMEMORY;
  const HRESULT result = service->QueryInterface(iid, object);
  service->Release();
  return result;
}

} // namespace famo::tsf
