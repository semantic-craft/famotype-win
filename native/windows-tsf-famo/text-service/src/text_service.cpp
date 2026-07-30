#include "text_service.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <new>
#include <utility>

#include <bcrypt.h>

#include "abi_boundary.h"
#include "famo_guids.h"
#include "famo_install_state.h"
#include "module_state.h"

namespace famo::tsf {

namespace {

std::atomic<uint64_t> g_activation_generation{0};
constexpr UINT kRecoveryMessage = WM_APP + 0x46;
constexpr int kSessionOpenAttempts = 3;
constexpr std::chrono::milliseconds kSessionRetryDelay{20};
constexpr size_t kMaxTerminalAbandonDebts = 64;
constexpr size_t kMaxTerminalDebtRetriesPerConnect = 4;

struct TerminalAbandonDebt {
  std::wstring endpoint_suffix;
  std::wstring runtime_executable;
  runtime::Correlation identity;
};

std::mutex g_terminal_debt_mutex;
std::deque<TerminalAbandonDebt> g_terminal_abandon_debts;
std::atomic<uint32_t> g_terminal_cleanup_connect_attempts{0};

bool SameConnectionEpoch(const runtime::Correlation &left,
                         const runtime::Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation;
}

runtime::Correlation ConnectionIdentity(runtime::Correlation identity) {
  identity.session_id = 0;
  identity.session_generation = 0;
  identity.sequence = 0;
  return identity;
}

void RememberTerminalAbandonDebt(
    std::wstring_view endpoint_suffix,
    std::wstring_view runtime_executable,
    const runtime::Correlation &identity) noexcept {
  try {
    const runtime::Correlation connection = ConnectionIdentity(identity);
    std::lock_guard lock(g_terminal_debt_mutex);
    const auto existing = std::find_if(
        g_terminal_abandon_debts.begin(), g_terminal_abandon_debts.end(),
        [&](const TerminalAbandonDebt &entry) {
          return entry.endpoint_suffix == endpoint_suffix &&
                 entry.runtime_executable == runtime_executable &&
                 SameConnectionEpoch(entry.identity, connection);
        });
    if (existing != g_terminal_abandon_debts.end())
      return;
    if (g_terminal_abandon_debts.size() >= kMaxTerminalAbandonDebts)
      g_terminal_abandon_debts.pop_front();
    g_terminal_abandon_debts.push_back(
        {std::wstring(endpoint_suffix), std::wstring(runtime_executable),
         connection});
  } catch (...) {
    // The runtime process is still bounded independently. Debt is a
    // best-effort same-process retry aid, never the authority for correctness.
  }
}

std::optional<TerminalAbandonDebt> TakeTerminalAbandonDebt(
    std::wstring_view endpoint_suffix,
    std::wstring_view runtime_executable) noexcept {
  try {
    std::lock_guard lock(g_terminal_debt_mutex);
    const auto found = std::find_if(
        g_terminal_abandon_debts.begin(), g_terminal_abandon_debts.end(),
        [&](const TerminalAbandonDebt &entry) {
          return entry.endpoint_suffix == endpoint_suffix &&
                 entry.runtime_executable == runtime_executable;
        });
    if (found == g_terminal_abandon_debts.end())
      return std::nullopt;
    TerminalAbandonDebt debt = std::move(*found);
    g_terminal_abandon_debts.erase(found);
    return debt;
  } catch (...) {
    return std::nullopt;
  }
}

bool SendTerminalAbandon(
    const runtime::PipeEndpoint &endpoint, std::wstring_view expected_runtime,
    const runtime::Correlation &identity,
    const std::atomic<bool> *cancelled = nullptr) {
  runtime::PipeRuntimePort port{kBridgeAbiVersion};
  std::string error;
  const runtime::Correlation connection = ConnectionIdentity(identity);
  if (!port.Connect(endpoint, expected_runtime, connection,
                    std::chrono::milliseconds(100), &error, cancelled)) {
    return false;
  }
  runtime::Frame abandon;
  abandon.command = runtime::Command::AbandonConnection;
  abandon.correlation = connection;
  const runtime::CallResult result =
      port.Call(std::move(abandon), runtime::kHardCallDeadline);
  port.Stop();
  return result.status == runtime::Status::Ok ||
         result.status == runtime::Status::StaleRequest;
}

void RetryTerminalAbandonDebts(
    std::wstring_view endpoint_suffix,
    std::wstring_view runtime_executable,
    const runtime::PipeEndpoint &endpoint, std::wstring_view expected_runtime,
    const std::atomic<bool> *cancelled) {
  for (size_t attempt = 0;
       attempt < kMaxTerminalDebtRetriesPerConnect; ++attempt) {
    std::optional<TerminalAbandonDebt> debt =
        TakeTerminalAbandonDebt(endpoint_suffix, runtime_executable);
    if (!debt)
      return;
    if (SendTerminalAbandon(endpoint, expected_runtime, debt->identity,
                            cancelled)) {
      continue;
    }
    RememberTerminalAbandonDebt(debt->endpoint_suffix,
                                debt->runtime_executable, debt->identity);
    return;
  }
}

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

bool SameLogicalSession(const runtime::Correlation &left,
                        const runtime::Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation &&
         left.session_id == right.session_id &&
         left.session_generation == right.session_generation;
}

bool RefreshSelectionCapability(runtime::UiState *state) {
  if (!state)
    return false;
  runtime::SelectionCapability capability;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (BCryptGenRandom(
            nullptr, reinterpret_cast<PUCHAR>(&capability),
            static_cast<ULONG>(sizeof(capability)),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 &&
        capability) {
      state->selection_capability = capability;
      return true;
    }
  }
  state->selection_capability = {};
  return false;
}

} // namespace

uint32_t TerminalCleanupConnectAttemptsForTest() noexcept {
  return g_terminal_cleanup_connect_attempts.load();
}

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
  const HRESULT result = ComBoundary(
      [&] { return ActivateCore(thread_manager, client_id, true); });
  if (FAILED(result))
    ForceDeactivateCleanup();
  return result;
}

HRESULT TextService::ActivateForTest(ITfThreadMgr *thread_manager,
                                     TfClientId client_id) {
  const HRESULT result = ComBoundary(
      [&] { return ActivateCore(thread_manager, client_id, false); });
  if (FAILED(result))
    ForceDeactivateCleanup();
  return result;
}

bool TextService::PreviewSelectionStateForTest(
    HWND *target,
    runtime::PreviewSelectionRequest *request) const noexcept {
  if (!target || !request || !OnActivationThread() || !recovery_window_)
    return false;
  for (const auto &entry : contexts_) {
    if (entry->close_requested || !entry->ui_state.focused ||
        entry->selection_capability_sequence == 0 ||
        !entry->ui_state.selection_capability) {
      continue;
    }
    *target = recovery_window_;
    *request = {
        entry->state.session_identity(),
        entry->selection_capability_sequence,
        0,
        0,
        entry->ui_state.selection_capability,
    };
    return true;
  }
  return false;
}

HRESULT TextService::ActivateCore(ITfThreadMgr *thread_manager,
                                  TfClientId client_id,
                                  bool advise_key_sink) {
  if (!thread_manager || thread_manager_)
    return E_INVALIDARG;
  activation_thread_ = GetCurrentThreadId();
  client_id_ = client_id;
  thread_manager_ = ComPtr<ITfThreadMgr>(thread_manager);
  if (GetEnvironmentVariableA("FAMO_TEST_ACTIVATION_ALLOCATION_FAILURE",
                              nullptr, 0) != 0) {
    throw std::bad_alloc();
  }
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
  const HRESULT result = ComBoundary([&] { return DeactivateCore(); });
  ForceDeactivateCleanup();
  return result;
}

HRESULT TextService::DeactivateCore() {
  if (GetEnvironmentVariableA("FAMO_TEST_DEACTIVATE_ALLOCATION_FAILURE",
                              nullptr, 0) != 0) {
    throw std::bad_alloc();
  }
  for (auto &entry : contexts_) {
    entry->close_requested = true;
    SetFocused(entry.get(), false);
  }
  const auto delivery_drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  while (!contexts_.empty() &&
         std::chrono::steady_clock::now() < delivery_drain_deadline) {
    ProcessRecoveryWork();
    if (contexts_.empty())
      break;
    bool has_delivery = false;
    for (const auto &entry : contexts_) {
      has_delivery = has_delivery || entry->pending_delivery ||
                     entry->applied_delivery ||
                     entry->delivery_work_pending;
    }
    if (!has_delivery) {
      FinalizeClosingContexts();
      break;
    }
    Sleep(5);
  }

  StopSessionWorker();
  ApplyDeliveryResult();
  FinalizeClosingContexts();

  // TSF requires Deactivate to release every thread-manager reference before
  // returning. If exact delivery recovery still could not finish, terminate
  // the authenticated logical connection explicitly. This is not an ACK and
  // never claims that a commit reached the document.
  std::vector<runtime::Correlation> abandon_identities;
  try {
    abandon_identities.reserve(contexts_.size());
  } catch (...) {
  }
  const auto remember_identity =
      [&](const runtime::DeliveryReference &reference) {
        const runtime::Correlation connection =
            ConnectionIdentity(reference.correlation);
        const bool duplicate =
            std::any_of(abandon_identities.begin(),
                        abandon_identities.end(),
                        [&](const runtime::Correlation &known) {
                          return SameConnectionEpoch(known, connection);
                        });
        if (duplicate)
          return;
        try {
          abandon_identities.push_back(connection);
        } catch (...) {
          RememberTerminalAbandonDebt(runtime_endpoint_suffix_,
                                      runtime_executable_name_, connection);
        }
      };
  for (const auto &owned : contexts_) {
    if (owned->pending_delivery)
      remember_identity(*owned->pending_delivery);
    if (owned->applied_delivery)
      remember_identity(*owned->applied_delivery);
  }
  for (const runtime::Correlation &connection : abandon_identities) {
    runtime_port_.Stop();
    bool resolved = false;
    if (ConnectRuntime(connection, false)) {
      runtime::Frame abandon;
      abandon.command = runtime::Command::AbandonConnection;
      abandon.correlation = connection;
      const runtime::CallResult abandoned =
          runtime_port_.Call(std::move(abandon),
                             runtime::kHardCallDeadline);
      resolved = abandoned.status == runtime::Status::Ok ||
                 abandoned.status == runtime::Status::StaleRequest;
    }
    if (!resolved) {
      RememberTerminalAbandonDebt(runtime_endpoint_suffix_,
                                  runtime_executable_name_, connection);
      OutputDebugStringA(
          "FamoTextService terminal delivery abandon deferred\n");
    }
  }

  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    std::string confirmed_preedit = entry->recovery_preedit;
    const RecoveryPlan recovery = entry->state.Fail();
    if (confirmed_preedit.empty() && recovery.commit_preedit)
      confirmed_preedit = *recovery.commit_preedit;
    if (!confirmed_preedit.empty()) {
      (void)entry->composition.Recover(
          entry->context.get(), client_id_, confirmed_preedit,
          static_cast<ITfCompositionSink *>(this));
    }
    if (entry->candidates)
      entry->candidates->End();
    entry->composition.ResetBehaviorState();
    entry->ui_state.show_allowed = false;
    SetFocused(entry, false);
    if (entry->layout_sink_cookie != TF_INVALID_COOKIE) {
      ComPtr<ITfSource> source;
      if (SUCCEEDED(entry->context->QueryInterface(
              IID_ITfSource,
              reinterpret_cast<void **>(source.put())))) {
        source->UnadviseSink(entry->layout_sink_cookie);
      }
      entry->layout_sink_cookie = TF_INVALID_COOKIE;
    }
    entry->composition.End(entry->context.get(), client_id_);
    entry->state.Close();
  }
  contexts_.clear();
  StopRecoveryWindow();

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

void TextService::AbandonOutstandingDeliveriesNoexcept() noexcept {
  g_terminal_cleanup_connect_attempts.store(0);
  constexpr size_t kIdentitySlots = 4;
  const auto identity_at =
      [](const std::unique_ptr<ContextEntry> &owned, size_t index,
         runtime::Correlation *identity) {
        if (!owned || !identity)
          return false;
        switch (index) {
        case 0:
          if (!owned->pending_delivery)
            return false;
          *identity = owned->pending_delivery->correlation;
          return true;
        case 1:
          if (!owned->applied_delivery)
            return false;
          *identity = owned->applied_delivery->correlation;
          return true;
        case 2:
          if (owned->pending_session.client_id == 0)
            return false;
          *identity = owned->pending_session;
          return true;
        case 3:
          *identity = owned->state.session_identity();
          return identity->client_id != 0;
        default:
          return false;
        }
      };
  const auto already_seen =
      [&](size_t context_index, size_t identity_index,
          const runtime::Correlation &connection) {
        for (size_t earlier_context = 0;
             earlier_context <= context_index; ++earlier_context) {
          const auto &owned = contexts_[earlier_context];
          if (!owned)
            continue;
          const size_t limit =
              earlier_context == context_index ? identity_index
                                               : kIdentitySlots;
          for (size_t earlier_identity = 0; earlier_identity < limit;
               ++earlier_identity) {
            runtime::Correlation candidate;
            if (identity_at(owned, earlier_identity, &candidate) &&
                SameConnectionEpoch(candidate, connection)) {
              return true;
            }
          }
        }
        return false;
      };

  for (size_t context_index = 0; context_index < contexts_.size();
       ++context_index) {
    const auto &owned = contexts_[context_index];
    if (!owned)
      continue;
    for (size_t identity_index = 0; identity_index < kIdentitySlots;
         ++identity_index) {
      runtime::Correlation identity;
      if (!identity_at(owned, identity_index, &identity))
        continue;
      const runtime::Correlation connection = ConnectionIdentity(identity);
      if (already_seen(context_index, identity_index, connection))
        continue;
      g_terminal_cleanup_connect_attempts.fetch_add(1);
      bool resolved = false;
      try {
        if (ConnectRuntime(connection, false)) {
          runtime::Frame request;
          request.command = runtime::Command::AbandonConnection;
          request.correlation = connection;
          const runtime::CallResult result =
              runtime_port_.Call(std::move(request),
                                 runtime::kHardCallDeadline);
          resolved = result.status == runtime::Status::Ok ||
                     result.status == runtime::Status::StaleRequest;
        }
      } catch (...) {
      }
      if (!resolved) {
        RememberTerminalAbandonDebt(runtime_endpoint_suffix_,
                                    runtime_executable_name_, connection);
      }
    }
  }
}

void TextService::ForceDeactivateCleanup() noexcept {
  // Idempotent no-unwind release barrier for both normal deactivation and any
  // exception caught at the COM boundary.
  StopSessionWorker();
  AbandonOutstandingDeliveriesNoexcept();
  try {
    runtime_port_.Stop();
  } catch (...) {
  }
  try {
    StopRecoveryWindow();
  } catch (...) {
    recovery_window_ = nullptr;
    recovery_message_posted_ = false;
  }
  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    if (!entry)
      continue;
    try {
      if (entry->candidates)
        entry->candidates->End();
    } catch (...) {
    }
    try {
      entry->composition.ResetBehaviorState();
      entry->composition.End(entry->context.get(), client_id_);
    } catch (...) {
    }
    try {
      if (entry->layout_sink_cookie != TF_INVALID_COOKIE && entry->context) {
        ComPtr<ITfSource> source;
        if (SUCCEEDED(entry->context->QueryInterface(
                IID_ITfSource, reinterpret_cast<void **>(source.put())))) {
          source->UnadviseSink(entry->layout_sink_cookie);
        }
      }
    } catch (...) {
    }
    entry->layout_sink_cookie = TF_INVALID_COOKIE;
    try {
      entry->state.Close();
    } catch (...) {
    }
  }
  contexts_.clear();
  try {
    if (keystroke_manager_ && key_sink_advised_)
      keystroke_manager_->UnadviseKeyEventSink(client_id_);
  } catch (...) {
  }
  key_sink_advised_ = false;
  try {
    if (thread_manager_ && thread_sink_cookie_ != TF_INVALID_COOKIE) {
      ComPtr<ITfSource> source;
      if (SUCCEEDED(thread_manager_->QueryInterface(
              IID_ITfSource, reinterpret_cast<void **>(source.put())))) {
        source->UnadviseSink(thread_sink_cookie_);
      }
    }
  } catch (...) {
  }
  thread_sink_cookie_ = TF_INVALID_COOKIE;
  ui_manager_.reset();
  keystroke_manager_.reset();
  thread_manager_.reset();
  client_id_ = TF_CLIENTID_NULL;
  activation_thread_ = 0;
}

bool TextService::OnActivationThread() const {
  return activation_thread_ != 0 && activation_thread_ == GetCurrentThreadId();
}

bool TextService::RenewSelectionCapability(
    ContextEntry *entry, uint64_t composition_sequence) noexcept {
  if (!entry)
    return false;
  entry->selection_capability_sequence = 0;
  if (!RefreshSelectionCapability(&entry->ui_state))
    return false;
  entry->selection_capability_sequence = composition_sequence;
  return true;
}

bool TextService::ConnectRuntime(const runtime::Correlation &identity,
                                 bool retry_terminal_debt) {
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
  std::wstring expected;
  std::chrono::milliseconds deadline{500};
  if (runtime_executable_name_ == L"FamoRuntime.exe") {
    if (!runtime::ResolveProductionRuntime(&expected))
      return false;
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
  } else {
    expected = ModuleDirectory() + L"\\" + runtime_executable_name_;
  }
  if (retry_terminal_debt) {
    RetryTerminalAbandonDebts(
        runtime_endpoint_suffix_, runtime_executable_name_, endpoint, expected,
        &session_worker_stop_);
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
  if (entry && entry->close_requested)
    return S_FALSE;
  if (entry && entry->delivery_quarantined)
    return S_FALSE;
  if (entry && !ApplyDeferredDelivery(entry))
    return S_FALSE;
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
  {
    std::lock_guard lock(delivery_queue_mutex_);
    delivery_requests_.clear();
    delivery_results_.clear();
  }
  terminal_delivery_result_.store(nullptr);
  desired_session_.store(nullptr);
  try {
    session_worker_ = std::thread(&TextService::SessionWorkerMain, this);
  } catch (...) {
    return false;
  }
  return true;
}

void TextService::StopSessionWorker() noexcept {
  session_worker_stop_.store(true);
  session_retry_wake_.notify_all();
  try {
    runtime_port_.CancelConnect();
  } catch (...) {
  }
  session_worker_epoch_.fetch_add(1);
  session_worker_epoch_.notify_all();
  // An idle worker exits immediately after the producer stop above. Preserve
  // its authenticated lane so forced terminal cleanup can abandon the exact
  // epoch without a reconnect window. A genuinely blocked call is still
  // cancelled after this short grace period.
  if (session_worker_.joinable() &&
      WaitForSingleObject(session_worker_.native_handle(), 10) !=
          WAIT_OBJECT_0) {
    try {
      runtime_port_.CancelCall(true);
    } catch (...) {
    }
  }
  try {
    if (session_worker_.joinable())
      session_worker_.join();
  } catch (...) {
    try {
      if (session_worker_.joinable())
        session_worker_.detach();
    } catch (...) {
    }
  }
  session_request_.store(nullptr);
  session_result_.store(nullptr);
  terminal_delivery_result_.store(nullptr);
  try {
    std::lock_guard lock(delivery_queue_mutex_);
    delivery_requests_.clear();
  } catch (...) {
  }
  desired_session_.store(nullptr);
  session_disconnect_requested_.store(false);
  session_worker_stop_.store(false);
}

bool TextService::StartRecoveryWindow() {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = &TextService::RecoveryWindowProc;
  window_class.hInstance = ModuleHandle();
  window_class.lpszClassName = runtime::kPreviewSelectionWindowClass;
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  const std::wstring title = std::to_wstring(runtime_client_id_);
  HWND window = CreateWindowExW(
      0, runtime::kPreviewSelectionWindowClass, title.c_str(), 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, ModuleHandle(), this);
  if (!window)
    return false;
  CHANGEFILTERSTRUCT filter{sizeof(filter)};
  ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, &filter);
  recovery_window_ = window;
  recovery_message_posted_ = false;
  return true;
}

void TextService::StopRecoveryWindow() {
  recovery_message_posted_ = false;
  if (recovery_window_)
    DestroyWindow(recovery_window_);
  recovery_window_ = nullptr;
}

void TextService::PostRecoveryWork() {
  if (!recovery_window_ || recovery_message_posted_)
    return;
  recovery_message_posted_ =
      PostMessageW(recovery_window_, kRecoveryMessage, 0, 0) != FALSE;
}

void TextService::ProcessRecoveryWork() {
  recovery_message_posted_ = false;
  ApplyDeliveryResult();
  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    if (entry->recovery_cleanup_required &&
        !entry->recovery_preedit.empty()) {
      const HRESULT recovered = entry->composition.Recover(
          entry->context.get(), client_id_, entry->recovery_preedit,
          static_cast<ITfCompositionSink *>(this));
      if (SUCCEEDED(recovered)) {
        entry->recovery_cleanup_required = false;
        entry->recovery_preedit.clear();
      }
    }
    if (!entry->delivery_quarantined)
      (void)ApplyDeferredDelivery(entry);
    if (!entry->delivery_work_pending) {
      if (entry->pending_delivery && !entry->delivery_quarantined) {
        ScheduleDeliveryWork(entry, entry->pending_delivery_work,
                             *entry->pending_delivery);
      } else if (entry->applied_delivery &&
                 (!entry->ui_state.focused || entry->close_requested)) {
        ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack,
                             *entry->applied_delivery);
      }
    }
  }
  ApplySessionResult();
  FinalizeClosingContexts();
}

LRESULT CALLBACK TextService::RecoveryWindowProc(HWND window, UINT message,
                                                 WPARAM wparam,
                                                 LPARAM lparam) {
  return BoundaryOr<LRESULT>(FALSE, [&] {
    if (message == WM_NCCREATE) {
      const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
      if (!create)
        return static_cast<LRESULT>(FALSE);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto *service = reinterpret_cast<TextService *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!service)
      return DefWindowProcW(window, message, wparam, lparam);
    if (message == kRecoveryMessage) {
      service->ProcessRecoveryWork();
      return static_cast<LRESULT>(0);
    }
    if (message == WM_COPYDATA) {
      const auto *copy = reinterpret_cast<const COPYDATASTRUCT *>(lparam);
      if (!copy || copy->dwData != runtime::kPreviewSelectionCopyDataId ||
          copy->cbData != sizeof(runtime::PreviewSelectionRequest) ||
          !copy->lpData)
        return static_cast<LRESULT>(FALSE);
      return static_cast<LRESULT>(
          service->HandlePreviewSelection(
              reinterpret_cast<HWND>(wparam),
              *static_cast<const runtime::PreviewSelectionRequest *>(
                  copy->lpData))
              ? TRUE
              : FALSE);
    }
    if (message == WM_NCDESTROY) {
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      service->recovery_window_ = nullptr;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  });
}

void TextService::ScheduleSession(ContextEntry *entry,
                                  SessionWarmupReason reason) {
  if (!entry || entry->session_pending)
    return;
  if (!RenewSelectionCapability(entry, 0))
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

void TextService::ScheduleDeliveryWork(
    ContextEntry *entry, DeliveryWorkKind kind,
    const runtime::DeliveryReference &reference) {
  if (!entry || entry->delivery_work_pending || !session_worker_.joinable())
    return;
  entry->pending_delivery_work = kind;
  try {
    runtime::Correlation identity = reference.correlation;
    identity.sequence = 0;
    auto request = std::make_shared<const DeliveryWorkRequest>(
        DeliveryWorkRequest{identity, reference, kind});
    {
      std::lock_guard lock(delivery_queue_mutex_);
      if (delivery_requests_.size() >= kMaxQueuedDeliveryWork)
        return;
      entry->delivery_work_pending = true;
      try {
        delivery_requests_.push_back(std::move(request));
      } catch (...) {
        entry->delivery_work_pending = false;
        throw;
      }
    }
    session_worker_epoch_.fetch_add(1);
    session_worker_epoch_.notify_one();
    session_retry_wake_.notify_all();
  } catch (...) {
    entry->delivery_work_pending = false;
  }
}

TextService::DeliveryAttempt
TextService::SendDelivery(ContextEntry *entry, runtime::Frame &&request) {
  DeliveryAttempt attempt;
  attempt.reference = {request.command, request.correlation};
  if (!entry)
    return attempt;

  const bool acknowledges_previous =
      entry->applied_delivery &&
      SameLogicalSession(entry->applied_delivery->correlation,
                         request.correlation);
  if (acknowledges_previous)
    request.flags |= runtime::kFlagAcknowledgePrevious;

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + runtime::kHardCallDeadline;
  const runtime::CallResult prepared =
      runtime_port_.Prepare(std::move(request), deadline);
  const bool explicit_response =
      prepared.reply.flags == runtime::kFlagResponse &&
      prepared.reply.command == attempt.reference.command &&
      prepared.reply.correlation == attempt.reference.correlation;
  if (!explicit_response ||
      prepared.status != runtime::Status::Prepared) {
    attempt.state = explicit_response
                        ? DeliveryAttemptState::Rejected
                        : DeliveryAttemptState::PrepareUnknown;
    attempt.status = prepared.status;
    attempt.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    return attempt;
  }

  if (acknowledges_previous)
    entry->applied_delivery.reset();
  runtime::DeliveryResult executed =
      runtime_port_.ExecutePrepared(attempt.reference, deadline);
  attempt.elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
  attempt.status = executed.status;
  if (executed.status == runtime::Status::Ok) {
    attempt.state = DeliveryAttemptState::Final;
    attempt.final_reply = std::move(executed.final_reply);
  } else {
    attempt.state = DeliveryAttemptState::PreparedAmbiguous;
  }
  return attempt;
}

void TextService::SessionWorkerMain() noexcept {
  try {
  uint64_t observed_epoch = session_worker_epoch_.load();
  while (!session_worker_stop_.load()) {
    std::shared_ptr<const DeliveryWorkRequest> delivery;
    {
      std::lock_guard lock(delivery_queue_mutex_);
      if (!delivery_requests_.empty()) {
        delivery = std::move(delivery_requests_.front());
        delivery_requests_.pop_front();
      }
    }
    if (delivery)
      session_retry_wake_.notify_all();
    if (delivery) {
      ProcessDeliveryWork(delivery);
      observed_epoch = session_worker_epoch_.load();
      continue;
    }
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
        if (runtime_port_.state() != runtime::ChannelState::Ready)
          ready = ConnectRuntime(request->identity);
        if (!ready)
          break;
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
        const bool retryable =
            result.status == runtime::Status::Unavailable ||
            result.status == runtime::Status::Timeout;
        if (!retryable) {
          runtime_port_.Poison();
          break;
        }
        if (result.status == runtime::Status::Timeout)
          runtime_port_.Poison();
        if (attempt + 1 >= kSessionOpenAttempts) {
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
  } catch (...) {
    try {
      runtime_port_.Poison();
      session_result_.store(nullptr);
      desired_session_.store(nullptr);
      if (recovery_window_)
        PostMessageW(recovery_window_, kRecoveryMessage, 0, 0);
    } catch (...) {
    }
  }
}

void TextService::ProcessDeliveryWork(
    const std::shared_ptr<const DeliveryWorkRequest> &request) {
  if (!request || session_worker_stop_.load())
    return;
  const auto requeue_without_dropping = [&]() {
    std::unique_lock retry_lock(session_retry_mutex_);
    session_retry_wake_.wait_for(retry_lock, kSessionRetryDelay, [&] {
      return session_worker_stop_.load();
    });
    retry_lock.unlock();
    while (!session_worker_stop_.load()) {
      try {
        {
          std::lock_guard lock(delivery_queue_mutex_);
          // The worker has already removed this request. A concurrent producer
          // may refill the public bound, so this one exact retry is permitted
          // to occupy the single in-flight overflow slot.
          delivery_requests_.push_front(request);
        }
        session_worker_epoch_.fetch_add(1);
        session_worker_epoch_.notify_one();
        return;
      } catch (...) {
        std::unique_lock allocation_retry(session_retry_mutex_);
        session_retry_wake_.wait_for(
            allocation_retry, kSessionRetryDelay,
            [&] { return session_worker_stop_.load(); });
      }
    }
  };
  while (GetEnvironmentVariableA("FAMO_TEST_PAUSE_DELIVERY_RECOVERY",
                                 nullptr, 0) != 0 &&
         !session_worker_stop_.load()) {
    Sleep(1);
  }
  if (session_worker_stop_.load())
    return;

  runtime::Status status = runtime::Status::Unavailable;
  runtime::Frame final_reply;
  if (ConnectRuntime(request->identity) && !session_worker_stop_.load()) {
    const auto deadline =
        std::chrono::steady_clock::now() + runtime::kHardCallDeadline;
    if (request->kind == DeliveryWorkKind::Recover) {
      runtime::DeliveryResult result =
          runtime_port_.Claim(request->reference, deadline);
      if (result.status == runtime::Status::Prepared) {
        result = runtime_port_.ExecutePrepared(
            request->reference,
            std::chrono::steady_clock::now() +
                runtime::kHardCallDeadline);
      }
      status = result.status;
      if (status == runtime::Status::Ok)
        final_reply = std::move(result.final_reply);
    } else {
      status =
          runtime_port_
              .Ack(request->reference,
                   std::chrono::steady_clock::now() +
                       runtime::kHardCallDeadline)
              .status;
    }
  }

  if (status == runtime::Status::DeliveryFailed) {
    std::shared_ptr<const DeliveryWorkResult> terminal_result;
    try {
      if (GetEnvironmentVariableA(
              "FAMO_TEST_TERMINAL_RESULT_ALLOCATION_FAILURE_ONCE", nullptr,
              0) != 0) {
        SetEnvironmentVariableA(
            "FAMO_TEST_TERMINAL_RESULT_ALLOCATION_FAILURE_ONCE", nullptr);
        throw std::bad_alloc();
      }
      terminal_result = std::make_shared<const DeliveryWorkResult>(
          DeliveryWorkResult{request->identity, request->reference,
                             request->kind, status, {}});
    } catch (...) {
      requeue_without_dropping();
      return;
    }

    // Reserve the allocation-free publication slot before the terminal
    // mutation. A stopped/deactivating host leaves the runtime delivery intact
    // for ForceDeactivateCleanup's connection-wide teardown.
    while (terminal_delivery_result_.load() &&
           !session_worker_stop_.load()) {
      std::unique_lock publication_wait(session_retry_mutex_);
      session_retry_wake_.wait_for(publication_wait, kSessionRetryDelay, [&] {
        return session_worker_stop_.load() ||
               !terminal_delivery_result_.load();
      });
    }
    if (session_worker_stop_.load())
      return;

    const runtime::CallResult abandoned = runtime_port_.AbandonSession(
        request->reference,
        std::chrono::steady_clock::now() + runtime::kHardCallDeadline);
    if (abandoned.status != runtime::Status::Ok &&
        abandoned.status != runtime::Status::StaleRequest) {
      requeue_without_dropping();
      return;
    }

    terminal_delivery_result_.store(std::move(terminal_result));
    if (recovery_window_)
      PostMessageW(recovery_window_, kRecoveryMessage, 0, 0);
    return;
  }

  const bool retryable =
      status == runtime::Status::Unavailable ||
      status == runtime::Status::Timeout ||
      status == runtime::Status::RecoveryPending;
  if (retryable && !session_worker_stop_.load()) {
    std::unique_lock retry_lock(session_retry_mutex_);
    session_retry_wake_.wait_for(retry_lock, kSessionRetryDelay, [&] {
      return session_worker_stop_.load();
    });
    if (!session_worker_stop_.load()) {
      bool requeued = false;
      try {
        std::lock_guard lock(delivery_queue_mutex_);
        if (delivery_requests_.size() < kMaxQueuedDeliveryWork) {
          delivery_requests_.push_back(request);
          requeued = true;
        }
      } catch (...) {
      }
      if (requeued) {
        session_worker_epoch_.fetch_add(1);
        session_worker_epoch_.notify_one();
        return;
      }
    }
  }
  if (session_worker_stop_.load())
    return;

  try {
    auto result = std::make_shared<const DeliveryWorkResult>(
        DeliveryWorkResult{request->identity, request->reference,
                           request->kind, status, std::move(final_reply)});
    bool published = false;
    while (!published && !session_worker_stop_.load()) {
      {
        std::lock_guard lock(delivery_queue_mutex_);
        if (delivery_results_.size() < kMaxQueuedDeliveryWork) {
          delivery_results_.push_back(result);
          published = true;
        }
      }
      if (!published) {
        std::unique_lock retry_lock(session_retry_mutex_);
        session_retry_wake_.wait_for(retry_lock, kSessionRetryDelay, [&] {
          return session_worker_stop_.load();
        });
      }
    }
    if (published && recovery_window_)
      PostMessageW(recovery_window_, kRecoveryMessage, 0, 0);
  } catch (...) {
    requeue_without_dropping();
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

void TextService::ApplyDeliveryResult() {
  for (;;) {
    std::shared_ptr<const DeliveryWorkResult> result =
        terminal_delivery_result_.exchange(nullptr);
    if (!result) {
      std::lock_guard lock(delivery_queue_mutex_);
      if (delivery_results_.empty())
        break;
      result = std::move(delivery_results_.front());
      delivery_results_.pop_front();
    }
    session_retry_wake_.notify_all();
    if (result)
      ApplyOneDeliveryResult(*result);
  }
}

void TextService::ApplyOneDeliveryResult(
    const DeliveryWorkResult &delivery_result) {
  const DeliveryWorkResult *result = &delivery_result;
  if (result->identity.activation_generation != activation_generation_)
    return;
  if (result->status == runtime::Status::DeliveryFailed) {
    RetireAbandonedSession(result->reference);
    return;
  }
  if (result->identity.connection_generation != connection_generation_)
    return;

  ContextEntry *entry = nullptr;
  for (auto &owned : contexts_) {
    if (SameLogicalSession(owned->state.session_identity(),
                           result->reference.correlation)) {
      entry = owned.get();
      break;
    }
  }
  if (!entry)
    return;

  entry->delivery_work_pending = false;
  if (result->kind == DeliveryWorkKind::Ack) {
    if (entry->applied_delivery &&
        *entry->applied_delivery == result->reference &&
        (result->status == runtime::Status::Ok ||
         result->status == runtime::Status::StaleRequest)) {
      entry->applied_delivery.reset();
    } else if (entry->applied_delivery &&
               *entry->applied_delivery == result->reference &&
               result->status != runtime::Status::StaleRequest) {
      ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack,
                           result->reference);
    }
    if (entry->recover_after_delivery_ack &&
        !entry->delivery_work_pending) {
      entry->recover_after_delivery_ack = false;
      RecoverConnection();
      return;
    }
    if (!entry->ui_state.focused)
      DisconnectRuntimeIfIdle();
    return;
  }

  if (!entry->pending_delivery ||
      *entry->pending_delivery != result->reference ||
      entry->pending_delivery_work != result->kind) {
    return;
  }

  if (result->kind == DeliveryWorkKind::Cancel) {
    if (result->status == runtime::Status::Unavailable ||
        result->status == runtime::Status::Timeout ||
        result->status == runtime::Status::RecoveryPending) {
      ScheduleDeliveryWork(entry, DeliveryWorkKind::Cancel,
                           result->reference);
      return;
    }
    const bool cancelled = result->status == runtime::Status::Ok;
    // The recovery worker addresses the exact logical delivery identity. A
    // StaleRequest from that authenticated runtime is authoritative evidence
    // that an unconfirmed Prepare never became durable; it is therefore safe
    // to pass the original key and continue the same session.
    const bool safe_missing =
        result->status == runtime::Status::StaleRequest;
    entry->pending_delivery.reset();
    if (cancelled || safe_missing) {
      entry->state.CompleteUnhandled();
      if (entry->pending_physical_key) {
        entry->composition.ObserveUnhandledKey(entry->pending_windows_key,
                                               entry->pending_key_down);
      }
      entry->pending_physical_key = false;
      if (!entry->ui_state.focused && entry->applied_delivery) {
        ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack,
                             *entry->applied_delivery);
      }
      return;
    }
    entry->pending_physical_key = false;
    if (entry->applied_delivery) {
      entry->recover_after_delivery_ack = true;
      ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack,
                           *entry->applied_delivery);
      return;
    }
    RecoverConnection();
    return;
  }

  if (result->status == runtime::Status::Unavailable ||
      result->status == runtime::Status::Timeout ||
      result->status == runtime::Status::RecoveryPending) {
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Recover,
                         result->reference);
    return;
  }
  if (result->status != runtime::Status::Ok ||
      result->final_reply.flags != runtime::kFlagResponse ||
      result->final_reply.command != result->reference.command ||
      result->final_reply.correlation != result->reference.correlation ||
      !entry->state.AcceptReply(result->final_reply.correlation)) {
    entry->pending_delivery.reset();
    entry->pending_physical_key = false;
    RecoverConnection();
    return;
  }

  if (result->final_reply.status != runtime::Status::Ok) {
    QuarantineDelivery(entry);
    return;
  }

  runtime::Composition composition;
  std::string error;
  if (!runtime::DecodeComposition(result->final_reply.payload, &composition,
                                  &error)) {
    QuarantineDelivery(entry);
    return;
  }
  if (composition.handled) {
    if (FAILED(ApplyRuntimeComposition(entry, composition))) {
      entry->deferred_delivery_composition = std::move(composition);
      return;
    }
    entry->state.ApplySucceeded(composition);
    UpdateCandidates(entry, composition);
  } else {
    // The original callback conservatively swallowed an ambiguous prepared
    // key, so this late unhandled result cannot be passed through retroactively.
    entry->state.CompleteUnhandled();
  }
  entry->pending_delivery.reset();
  entry->pending_physical_key = false;
  entry->applied_delivery = result->reference;
  if (!entry->ui_state.focused) {
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack, result->reference);
  }
}

void TextService::RetireAbandonedSession(
    const runtime::DeliveryReference &reference) {
  try {
    std::lock_guard lock(delivery_queue_mutex_);
    std::erase_if(
        delivery_requests_,
        [&](const std::shared_ptr<const DeliveryWorkRequest> &request) {
          return request &&
                 SameLogicalSession(request->reference.correlation,
                                    reference.correlation);
        });
    std::erase_if(
        delivery_results_,
        [&](const std::shared_ptr<const DeliveryWorkResult> &result) {
          return result &&
                 SameLogicalSession(result->reference.correlation,
                                    reference.correlation);
        });
  } catch (...) {
  }
  session_retry_wake_.notify_all();

  ContextEntry *retired = nullptr;
  for (auto &owned : contexts_) {
    ContextEntry *entry = owned.get();
    if (!SameLogicalSession(entry->state.session_identity(),
                            reference.correlation) &&
        (!entry->pending_delivery ||
         !SameLogicalSession(entry->pending_delivery->correlation,
                             reference.correlation)) &&
        (!entry->applied_delivery ||
         !SameLogicalSession(entry->applied_delivery->correlation,
                             reference.correlation))) {
      continue;
    }
    retired = entry;
    if (entry->candidates)
      entry->candidates->End();
    entry->ui_state.show_allowed = false;
    entry->composition.ResetBehaviorState();
    const RecoveryPlan recovery = entry->state.Fail();
    if (recovery.commit_preedit) {
      entry->recovery_preedit = *recovery.commit_preedit;
      entry->recovery_cleanup_required = true;
    }
    entry->pending_delivery.reset();
    entry->applied_delivery.reset();
    entry->deferred_delivery_composition.reset();
    entry->pending_physical_key = false;
    entry->delivery_work_pending = false;
    entry->delivery_quarantined = false;
    entry->recover_after_delivery_ack = false;
    entry->selection_capability_sequence = 0;
    {
      std::lock_guard lock(session_publication_mutex_);
      const std::shared_ptr<const runtime::Correlation> desired =
          desired_session_.load();
      if (desired && *desired == entry->pending_session) {
        desired_session_.store(nullptr);
        session_request_.store(nullptr);
        const std::shared_ptr<const SessionWarmupResult> result =
            session_result_.load();
        if (result && result->identity == entry->pending_session)
          session_result_.store(nullptr);
      }
      entry->session_pending = false;
      entry->pending_session = {};
    }
    break;
  }
  if (!retired)
    return;
  PostRecoveryWork();
  if (retired->ui_state.focused)
    ScheduleSession(retired, SessionWarmupReason::Recovery);
}

bool TextService::ApplyDeferredDelivery(ContextEntry *entry) {
  if (!entry || !entry->deferred_delivery_composition)
    return true;
  if (entry->delivery_quarantined || !entry->pending_delivery ||
      !entry->state.AcceptReply(
          entry->pending_delivery->correlation)) {
    QuarantineDelivery(entry);
    return false;
  }
  if (FAILED(
          ApplyRuntimeComposition(entry, *entry->deferred_delivery_composition)))
    return false;
  const runtime::DeliveryReference completed = *entry->pending_delivery;
  entry->state.ApplySucceeded(*entry->deferred_delivery_composition);
  UpdateCandidates(entry, *entry->deferred_delivery_composition);
  entry->deferred_delivery_composition.reset();
  entry->pending_delivery.reset();
  entry->pending_physical_key = false;
  entry->applied_delivery = completed;
  if (!entry->ui_state.focused)
    ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack, completed);
  return true;
}

void TextService::QuarantineDelivery(ContextEntry *entry) {
  if (!entry || entry->delivery_quarantined)
    return;
  entry->delivery_quarantined = true;
  entry->delivery_work_pending = false;
  entry->deferred_delivery_composition.reset();
  entry->pending_physical_key = false;
  if (entry->candidates)
    entry->candidates->End();
  entry->ui_state.show_allowed = false;
  entry->composition.ResetBehaviorState();
  const RecoveryPlan recovery = entry->state.Fail();
  if (recovery.commit_preedit) {
    entry->recovery_preedit = *recovery.commit_preedit;
    entry->recovery_cleanup_required = true;
  }
  // Retain pending_delivery and do not ACK it. The engine action must never be
  // replayed, while a malformed snapshot or failed host contract must not be
  // mistaken for an applied commit.
  runtime_port_.Poison();
  PostRecoveryWork();
}

void TextService::DisconnectRuntimeIfIdle() {
  for (auto &entry : contexts_) {
    if (entry->pending_delivery || entry->delivery_work_pending)
      return;
    if (entry->applied_delivery) {
      ScheduleDeliveryWork(entry.get(), DeliveryWorkKind::Ack,
                           *entry->applied_delivery);
      return;
    }
    if (entry->ui_state.focused || !entry->state.displayed().preedit.empty() ||
        entry->state.pending_sequence() != 0) {
      return;
    }
  }
  {
    std::lock_guard lock(session_publication_mutex_);
    desired_session_.store(nullptr);
    session_request_.store(nullptr);
    session_result_.store(nullptr);
    for (auto &entry : contexts_) {
      entry->session_pending = false;
      entry->pending_session = {};
      entry->state.Close();
    }
  }
  session_disconnect_requested_.store(true);
  session_worker_epoch_.fetch_add(1);
  session_worker_epoch_.notify_one();
  session_retry_wake_.notify_all();
}

TextService::ContextEntry *TextService::FindContext(ITfContext *context) {
  const auto found = std::find_if(
      contexts_.begin(), contexts_.end(), [&](const auto &entry) {
        return SameComObject(entry->context.get(), context);
      });
  return found == contexts_.end() ? nullptr : found->get();
}

void TextService::CloseContext(ITfContext *context) {
  ApplyDeliveryResult();
  const auto found = std::find_if(
      contexts_.begin(), contexts_.end(), [&](const auto &entry) {
        return SameComObject(entry->context.get(), context);
      });
  if (found == contexts_.end())
    return;
  (*found)->close_requested = true;
  SetFocused(found->get(), false);
  if (CloseEntry(found->get())) {
    contexts_.erase(found);
  } else {
    PostRecoveryWork();
  }
}

void TextService::FinalizeClosingContexts() {
  for (size_t index = 0; index < contexts_.size();) {
    ContextEntry *entry = contexts_[index].get();
    if (entry->close_requested && CloseEntry(entry)) {
      contexts_.erase(contexts_.begin() + static_cast<ptrdiff_t>(index));
    } else {
      ++index;
    }
  }
}

bool TextService::CloseEntry(ContextEntry *entry) {
  if (!entry)
    return true;
  entry->close_requested = true;
  SetFocused(entry, false);
  if (entry->candidates)
    entry->candidates->End();
  entry->ui_state.show_allowed = false;

  if (!entry->delivery_quarantined &&
      !ApplyDeferredDelivery(entry)) {
    return false;
  }
  if (entry->pending_delivery) {
    if (!entry->delivery_quarantined && !entry->delivery_work_pending) {
      ScheduleDeliveryWork(entry, entry->pending_delivery_work,
                           *entry->pending_delivery);
    }
    return false;
  }
  if (entry->applied_delivery) {
    if (!entry->delivery_work_pending) {
      ScheduleDeliveryWork(entry, DeliveryWorkKind::Ack,
                           *entry->applied_delivery);
    }
    return false;
  }
  if (entry->delivery_work_pending)
    return false;

  {
    std::lock_guard lock(session_publication_mutex_);
    const std::shared_ptr<const runtime::Correlation> desired =
        desired_session_.load();
    if (desired && *desired == entry->pending_session)
      desired_session_.store(nullptr);
    entry->session_pending = false;
    entry->pending_session = {};
  }
  entry->composition.ResetBehaviorState();
  entry->recovery_cleanup_required = false;
  entry->recovery_preedit.clear();
  if (entry->layout_sink_cookie != TF_INVALID_COOKIE) {
    ComPtr<ITfSource> source;
    if (SUCCEEDED(entry->context->QueryInterface(
            IID_ITfSource, reinterpret_cast<void **>(source.put()))))
      source->UnadviseSink(entry->layout_sink_cookie);
    entry->layout_sink_cookie = TF_INVALID_COOKIE;
  }
  entry->composition.End(entry->context.get(), client_id_);
  if (runtime_port_.state() == runtime::ChannelState::Ready) {
    const auto correlation = entry->state.PlanClose();
    if (correlation) {
      runtime::Frame close;
      close.command = runtime::Command::CloseSession;
      close.correlation = *correlation;
      if (runtime_port_.Call(std::move(close), runtime::kHardCallDeadline)
              .status != runtime::Status::Ok) {
        runtime_port_.Poison();
      }
    }
  }
  entry->state.Close();
  return true;
}

HRESULT TextService::OnSetFocus(BOOL foreground) {
  return BoundaryOr<HRESULT>(S_OK, [&] {
  if (!thread_manager_ || !OnActivationThread())
    return S_OK;
  ApplySessionResult();
  if (!foreground) {
    for (auto &entry : contexts_)
      SetFocused(entry.get(), false);
    DisconnectRuntimeIfIdle();
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
  });
}

HRESULT TextService::OnInitDocumentMgr(ITfDocumentMgr *) { return S_OK; }

HRESULT TextService::OnUninitDocumentMgr(ITfDocumentMgr *document) {
  return BoundaryOr<HRESULT>(S_OK, [&] {
  for (size_t index = 0; index < contexts_.size();) {
    if (SameComObject(contexts_[index]->document.get(), document)) {
      contexts_[index]->close_requested = true;
      SetFocused(contexts_[index].get(), false);
      if (CloseEntry(contexts_[index].get())) {
        contexts_.erase(contexts_.begin() + static_cast<ptrdiff_t>(index));
      } else {
        ++index;
      }
    } else {
      ++index;
    }
  }
  return S_OK;
  });
}

HRESULT TextService::OnSetFocus(ITfDocumentMgr *focus,
                                ITfDocumentMgr *previous) {
  return ComBoundary([&] {
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
  });
}

HRESULT TextService::OnPushContext(ITfContext *context) {
  return ComBoundary([&] { return EnsureContext(context); });
}

HRESULT TextService::OnPopContext(ITfContext *context) {
  return ComBoundary([&] {
  ComPtr<ITfDocumentMgr> document;
  if (context)
    context->GetDocumentMgr(document.put());
  CloseContext(context);
  ComPtr<ITfContext> top = TopContext(document.get());
  if (!top || SameComObject(top.get(), context)) {
    const auto previous = std::find_if(
        contexts_.rbegin(), contexts_.rend(), [&](const auto &entry) {
          return !entry->close_requested &&
                 SameComObject(entry->document.get(), document.get());
        });
    top = previous == contexts_.rend()
              ? ComPtr<ITfContext>()
              : ComPtr<ITfContext>((*previous)->context.get());
  }
  if (top)
    EnsureContext(top.get());
  return S_OK;
  });
}

HRESULT TextService::OnCompositionTerminated(TfEditCookie,
                                             ITfComposition *composition) {
  return BoundaryOr<HRESULT>(S_OK, [&] {
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
  });
}

HRESULT CreateTextServiceInstance(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  return ComBoundary([&] {
    auto *service = new (std::nothrow) TextService();
    if (!service)
      return E_OUTOFMEMORY;
    const HRESULT result = service->QueryInterface(iid, object);
    service->Release();
    return result;
  });
}

} // namespace famo::tsf
