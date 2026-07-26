#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <msctf.h>

#include "candidate_ui_element.h"
#include "com_ptr.h"
#include "composition_controller.h"
#include "famo_runtime_pipe.h"
#include "famo_tsf_host_model.h"

namespace famo::tsf {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfCompositionSink,
                          public ITfTextLayoutSink {
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

  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
      TfEditCookie cookie, ITfComposition *composition) override;
  HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext *context,
                                           TfLayoutCode code,
                                           ITfContextView *view) override;

private:
  struct ContextEntry {
    ComPtr<ITfContext> context;
    ComPtr<ITfDocumentMgr> document;
    ContextState state;
    CompositionController composition;
    ComPtr<CandidateUiElement> candidates;
    DWORD layout_sink_cookie = TF_INVALID_COOKIE;
    runtime::UiState ui_state;
    runtime::Correlation pending_session;
    std::string recovery_preedit;
    bool recovery_cleanup_required = false;
    bool session_pending = false;
    bool first_key_pending = true;
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

  class LayoutEditSession;

  ~TextService();
  TextService(std::wstring runtime_endpoint_suffix,
              std::wstring runtime_executable_name, std::string schema_id);
  HRESULT ActivateCore(ITfThreadMgr *thread_manager, TfClientId client_id,
                       bool advise_key_sink);
  bool OnActivationThread() const;
  bool ConnectRuntime(const runtime::Correlation &identity);
  HRESULT EnsureContext(
      ITfContext *context,
      SessionWarmupReason reason = SessionWarmupReason::Focus);
  bool StartSessionWorker();
  void StopSessionWorker();
  bool StartRecoveryWindow();
  void StopRecoveryWindow();
  void PostRecoveryWork();
  void ProcessRecoveryWork();
  static LRESULT CALLBACK RecoveryWindowProc(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);
  void SessionWorkerMain();
  void ScheduleSession(ContextEntry *entry, SessionWarmupReason reason);
  void ApplySessionResult();
  void DisconnectRuntimeIfIdle();
  void ReportTiming(const char *operation,
                    std::chrono::milliseconds elapsed,
                    const runtime::Correlation &identity,
                    runtime::Status status) const;
  ContextEntry *FindContext(ITfContext *context);
  void CloseContext(ITfContext *context);
  void CloseEntry(ContextEntry *entry);
  HRESULT HandleKey(ITfContext *context, WPARAM key, LPARAM key_data,
                    bool down, bool test_only, BOOL *eaten);
  bool HandlePreviewSelection(const runtime::PreviewSelectionRequest &request);
  HRESULT ApplyRuntimeComposition(ContextEntry *entry,
                                  const runtime::Composition &composition);
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
  runtime::PipeRuntimePort runtime_port_;
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
  std::mutex session_retry_mutex_;
  std::condition_variable session_retry_wake_;
  HWND recovery_window_ = nullptr;
  bool recovery_message_posted_ = false;
};

HRESULT CreateTextServiceInstance(REFIID iid, void **object);

} // namespace famo::tsf
