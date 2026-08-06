#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <msctf.h>
#include <ctffunc.h>

#include "candidate_ui_element.h"
#include "candidate_window.h"
#include "com_ptr.h"
#include "composition_controller.h"
#include "famo_bridge_abi.h"
#include "famo_runtime_pipe.h"
#include "famo_tsf_host_model.h"

namespace famo::tsf {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfCompartmentEventSink,
                          public ITfCompositionSink,
                          public ITfTextLayoutSink,
                          public ITfFunctionProvider,
                          public ITfFnSearchCandidateProvider,
                          public ITfDisplayAttributeProvider,
                          public CandidateUiHost {
public:
  TextService();
  explicit TextService(std::wstring runtime_endpoint_suffix);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr *thread_manager,
                                     TfClientId client_id) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;
  HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr *thread_manager,
                                       TfClientId client_id,
                                       DWORD flags) override;
  HRESULT ActivateForTest(ITfThreadMgr *thread_manager, TfClientId client_id);
  bool PreviewSelectionStateForTest(
      HWND *target, runtime::PreviewSelectionRequest *request) const noexcept;
  bool UiStateForTest(ITfContext *context,
                      runtime::UiState *state) const noexcept;

  HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext *context, WPARAM key,
                                          LPARAM key_data,
                                          BOOL *eaten) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext *context, WPARAM key,
                                        LPARAM key_data, BOOL *eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext *context, WPARAM key,
                                      LPARAM key_data, BOOL *eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext *context, WPARAM key,
                                    LPARAM key_data, BOOL *eaten) override;
  HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext *context, REFGUID guid,
                                           BOOL *eaten) override;

  HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr *document) override;
  HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr *document) override;
  HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr *focus,
                                       ITfDocumentMgr *previous) override;
  HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext *context) override;
  HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext *context) override;

  HRESULT STDMETHODCALLTYPE OnChange(REFGUID guid) override;

  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
      TfEditCookie cookie, ITfComposition *composition) override;
  HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext *context,
                                           TfLayoutCode code,
                                           ITfContextView *view) override;

  HRESULT STDMETHODCALLTYPE GetType(GUID *guid) override;
  HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override;
  HRESULT STDMETHODCALLTYPE GetFunction(REFGUID guid, REFIID iid,
                                        IUnknown **function) override;
  HRESULT STDMETHODCALLTYPE GetDisplayName(BSTR *name) override;
  HRESULT STDMETHODCALLTYPE GetSearchCandidates(
      BSTR query, BSTR application_id, ITfCandidateList **candidates) override;
  HRESULT STDMETHODCALLTYPE SetResult(BSTR query, BSTR application_id,
                                      BSTR result) override;

  HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(
      IEnumTfDisplayAttributeInfo **enumerator) override;
  HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(
      REFGUID guid, ITfDisplayAttributeInfo **info) override;

  void OnCandidateVisibilityChanged(CandidateUiElement *element) override;
  HRESULT OnCandidateBehavior(CandidateUiElement *element,
                              CandidateBehavior behavior, UINT index) override;
  HRESULT OnCandidateKeyDown(CandidateUiElement *element, WPARAM key,
                             LPARAM key_data, BOOL *eaten) override;

private:
  enum class DeliveryWorkKind { Recover, Cancel, Ack, Abandon };
  enum class KeyboardSecurityState { Enabled, Closing, Disabled };
  enum class DeliveryAttemptState {
    Rejected,
    PrepareUnknown,
    Final,
    PreparedAmbiguous
  };

  struct DeliveryAttempt {
    DeliveryAttemptState state = DeliveryAttemptState::Rejected;
    runtime::Status status = runtime::Status::Unavailable;
    runtime::DeliveryReference reference;
    runtime::Frame final_reply;
    std::chrono::milliseconds elapsed{0};
  };

  struct ExactCandidateCommit {
    runtime::Correlation correlation;
    std::string preedit;
  };

  struct ContextEntry {
    ComPtr<ITfContext> context;
    ComPtr<ITfDocumentMgr> document;
    ContextState state;
    CompositionController composition;
    ComPtr<CandidateUiElement> candidates;
    DWORD layout_sink_cookie = TF_INVALID_COOKIE;
    ComPtr<ITfSource> keyboard_disabled_source;
    DWORD keyboard_disabled_sink_cookie = TF_INVALID_COOKIE;
    runtime::UiState ui_state;
    HWND candidate_owner = nullptr;
    runtime::Correlation pending_session;
    std::string recovery_preedit;
    std::optional<runtime::DeliveryReference> pending_delivery;
    std::optional<runtime::DeliveryReference> applied_delivery;
    std::optional<runtime::Composition> deferred_delivery_composition;
    std::optional<ExactCandidateCommit> exact_candidate_commit;
    DeliveryWorkKind pending_delivery_work = DeliveryWorkKind::Recover;
    WPARAM pending_windows_key = 0;
    bool pending_key_down = false;
    bool pending_physical_key = false;
    bool delivery_work_pending = false;
    bool delivery_quarantined = false;
    bool recover_after_delivery_ack = false;
    bool recovery_cleanup_required = false;
    bool session_pending = false;
    bool first_key_pending = true;
    bool close_requested = false;
    KeyboardSecurityState keyboard_security = KeyboardSecurityState::Enabled;
    bool keyboard_reenable_requested = false;
    bool composition_cleanup_pending = false;
    uint8_t keyboard_security_retry_count = 0;
    // Capability is valid only for this exact displayed composition sequence.
    // Zero means the current composition has already consumed its click.
    uint64_t selection_capability_sequence = 0;

    // TSF AddRefs the element in BeginUIElement, so it can outlive this entry.
    // Detaching here covers every erase/clear site at once.
    ~ContextEntry() {
      if (candidates)
        candidates->SetHost(nullptr);
    }
  };

  enum class SessionWarmupReason { Activation, Focus, Recovery };

  struct SessionWarmupRequest {
    runtime::Correlation identity;
    SessionWarmupReason reason = SessionWarmupReason::Activation;
  };

  struct SessionWarmupResult {
    runtime::Correlation identity;
    bool ready = false;
  };

  struct DeliveryWorkRequest {
    runtime::Correlation identity;
    runtime::DeliveryReference reference;
    DeliveryWorkKind kind = DeliveryWorkKind::Recover;
  };

  struct DeliveryWorkResult {
    runtime::Correlation identity;
    runtime::DeliveryReference reference;
    DeliveryWorkKind kind = DeliveryWorkKind::Recover;
    runtime::Status status = runtime::Status::Unavailable;
    runtime::Frame final_reply;
  };

  class LayoutEditSession;

  ~TextService();
  TextService(std::wstring runtime_endpoint_suffix,
              std::wstring runtime_executable_name, std::string schema_id);
  HRESULT ActivateCore(ITfThreadMgr *thread_manager, TfClientId client_id,
                       bool advise_key_sink);
  HRESULT DeactivateCore();
  void AbandonOutstandingDeliveriesNoexcept() noexcept;
  void ForceDeactivateCleanup() noexcept;
  bool OnActivationThread() const;
  bool ConnectRuntime(const runtime::Correlation &identity,
                      bool retry_terminal_debt = true);
  bool ConnectRuntimePort(runtime::PipeRuntimePort *port,
                          const runtime::Correlation &identity,
                          bool retry_terminal_debt,
                          const std::atomic<bool> *cancelled);
  HRESULT EnsureContext(
      ITfContext *context,
      SessionWarmupReason reason = SessionWarmupReason::Focus);
  bool StartSessionWorker();
  void StopSessionWorker() noexcept;
  bool StartRecoveryWindow();
  void StopRecoveryWindow();
  void PostRecoveryWork();
  void ProcessRecoveryWork();
  static LRESULT CALLBACK RecoveryWindowProc(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);
  void SessionWorkerMain() noexcept;
  void ProcessDeliveryWork(
      const std::shared_ptr<const DeliveryWorkRequest> &request);
  void ScheduleSession(ContextEntry *entry, SessionWarmupReason reason);
  bool ScheduleDeliveryWork(ContextEntry *entry, DeliveryWorkKind kind,
                            const runtime::DeliveryReference &reference);
  void ApplySessionResult();
  void ApplyDeliveryResult();
  void ApplyOneDeliveryResult(const DeliveryWorkResult &result);
  void FinalizeClosingContexts();
  bool ApplyDeferredDelivery(ContextEntry *entry);
  void QuarantineDelivery(ContextEntry *entry);
  DeliveryAttempt SendDelivery(ContextEntry *entry, runtime::Frame &&request);
  void DisconnectRuntimeIfIdle();
  void ReportTiming(const char *operation,
                    std::chrono::milliseconds elapsed,
                    const runtime::Correlation &identity,
                    runtime::Status status) const;
  ContextEntry *FindContext(ITfContext *context);
  ContextEntry *FindContextByCandidateElement(CandidateUiElement *element);
  void CloseContext(ITfContext *context);
  bool CloseEntry(ContextEntry *entry);
  HRESULT HandleKey(ITfContext *context, WPARAM key, LPARAM key_data,
                    bool down, bool test_only, BOOL *eaten);
  bool KeyboardDisabled(ITfContext *context);
  void ReconcileKeyboardSecurity(ContextEntry *entry, bool disabled);
  void ContinueKeyboardSecurityClose(ContextEntry *entry);
  void ScheduleKeyboardSecurityRetry(ContextEntry *entry);
  void BlockSecurityConnection(uint64_t connection_generation);
  void ReleaseSecurityConnection(uint64_t connection_generation);
  void ResetSecurityConnections();
  bool SecurityConnectionBlockedLocked(
      uint64_t connection_generation) const noexcept;
  void CancelPendingSession(ContextEntry *entry);
  void UnadviseContextSinks(ContextEntry *entry);
  bool HandlePreviewSelection(
      HWND source_window,
      const runtime::PreviewSelectionRequest &request);
  bool DeliverCandidateRequest(ContextEntry *entry, runtime::Frame &&request,
                               std::string exact_commit = {});
  bool ResolveCandidateCommitOverride(
      ContextEntry *entry, const runtime::DeliveryReference &reference,
      const runtime::Composition &composition,
      const std::string **commit_override) const;
  static std::string_view HostInlinePreedit(
      const runtime::Composition &composition) noexcept;
  static std::string_view ExactCompositionText(
      const runtime::Composition &composition) noexcept;
  bool RenewSelectionCapability(ContextEntry *entry,
                                uint64_t composition_sequence) noexcept;
  HRESULT ApplyRuntimeComposition(ContextEntry *entry,
                                  const runtime::Composition &composition,
                                  const std::string *commit_override = nullptr)
      noexcept;
  void RetireAbandonedSession(
      const runtime::DeliveryReference &reference);
  void RecoverConnection();
  void UpdateCandidates(ContextEntry *entry,
                        const runtime::Composition &composition);
  void PublishUiState(ContextEntry *entry);
  void RefreshLayout(ContextEntry *entry, ITfContextView *view);
  HRESULT CaptureLayout(ITfContext *context, ITfContextView *view,
                        TfEditCookie cookie);
  void SetFocused(ContextEntry *entry, bool focused);
  HostKey MakeKey(WPARAM key, LPARAM key_data, bool down,
                  bool preserve_keyboard_state) const;

  std::atomic<ULONG> references_{1};
  DWORD activation_thread_ = 0;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_sink_cookie_ = TF_INVALID_COOKIE;
  bool key_sink_advised_ = false;
  ComPtr<ITfThreadMgr> thread_manager_;
  ComPtr<ITfKeystrokeMgr> keystroke_manager_;
  ComPtr<ITfUIElementMgr> ui_manager_;
  ComPtr<ITfSourceSingle> function_source_;
  bool function_provider_advised_ = false;
  runtime::PipeRuntimePort runtime_port_{kBridgeAbiVersion};
  std::wstring runtime_endpoint_suffix_;
  std::wstring runtime_executable_name_;
  std::string schema_id_;
  std::vector<std::unique_ptr<ContextEntry>> contexts_;
  uint64_t runtime_client_id_ = 0;
  uint64_t activation_generation_ = 0;
  uint64_t connection_generation_ = 0;
  uint64_t next_session_id_ = 1;
  uint64_t next_session_generation_ = 1;
  bool timing_enabled_ = false;
  std::thread session_worker_;
  std::atomic<bool> session_worker_stop_{false};
  std::atomic<bool> session_disconnect_requested_{false};
  std::atomic<uint64_t> session_worker_epoch_{0};
  std::atomic<std::shared_ptr<const SessionWarmupRequest>> session_request_;
  std::atomic<std::shared_ptr<const SessionWarmupResult>> session_result_;
  std::atomic<std::shared_ptr<const runtime::Correlation>> desired_session_;
  std::mutex session_publication_mutex_;
  static constexpr size_t kMaxQueuedDeliveryWork = 64;
  std::mutex delivery_queue_mutex_;
  // Serializes the security boundary with the only recovery operation that
  // may still execute a prepared engine action.
  std::mutex delivery_execution_mutex_;
  std::array<uint64_t, kMaxQueuedDeliveryWork>
      security_blocked_connections_{};
  bool security_blocks_all_recovery_ = false;
  std::deque<std::shared_ptr<const DeliveryWorkRequest>>
      delivery_requests_;
  std::deque<std::shared_ptr<const DeliveryWorkResult>>
      delivery_results_;
  // Security teardown gets one allocation-stable priority slot so it can
  // overtake an in-flight Recover/Cancel without growing the bounded queue.
  std::atomic<std::shared_ptr<const DeliveryWorkRequest>>
      security_abandon_request_;
  // A terminal result is allocated before its destructive runtime abandon and
  // published through this allocation-free slot afterward.
  std::atomic<std::shared_ptr<const DeliveryWorkResult>>
      terminal_delivery_result_;
  std::mutex session_retry_mutex_;
  std::condition_variable session_retry_wake_;
  HWND recovery_window_ = nullptr;
  bool recovery_message_posted_ = false;
  runtime::CandidateWindow candidate_window_;
  uint64_t candidate_revision_ = 0;
  TfGuidAtom display_attribute_atom_ = TF_INVALID_GUIDATOM;
  // Appearance the Runtime read from the user's data root and sent over the
  // pipe. The Bridge is loaded into hosts that cannot resolve or open that
  // root at all, so it never reads the overlay itself. Written by the session
  // worker after each connect, read by the activation thread when publishing.
  std::atomic<std::shared_ptr<const runtime::RuntimeStyleState>>
      runtime_style_;
  bool FetchRuntimeStyle();
};

HRESULT CreateTextServiceInstance(REFIID iid, void **object);
uint32_t TerminalCleanupConnectAttemptsForTest() noexcept;
uint32_t RecoveryPreparedClaimsForTest() noexcept;
uint32_t RecoveryExecuteAttemptsForTest() noexcept;
uint32_t TerminalPublicationReadyForTest() noexcept;
uint32_t TerminalRetiredSessionsForTest() noexcept;

} // namespace famo::tsf
