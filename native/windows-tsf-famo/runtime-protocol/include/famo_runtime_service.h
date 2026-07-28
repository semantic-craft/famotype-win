#pragma once

#include <atomic>
#include <compare>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../../engine-api/host/famo_engine_host.h"
#include "famo_pipe_security.h"
#include "famo_runtime_protocol.h"

namespace famo::runtime {

inline constexpr size_t kRuntimeClientCapacity = 64;
// One acceptor remains available to complete Hello and return protocol-level
// Unavailable after all persistent logical client slots are occupied.
inline constexpr size_t kRuntimeAcceptWorkerCapacity =
    kRuntimeClientCapacity + 1;

struct RuntimeStyleState {
  uint32_t host_behavior_flags = 0;
  std::shared_ptr<const void> presentation;
};

struct RuntimeSnapshot {
  Correlation correlation;
  Composition composition;
  UiState ui_state;
  // Runtime-process-only target captured when the focused snapshot is
  // published. It stays outside UiState because it is local process metadata,
  // not part of the v2 TSF-to-Runtime wire snapshot.
  uintptr_t source_window = 0;
  // Exact authenticated TSF process that owns the message-only selection
  // target. CandidateWindow never routes a capability to a same-name window
  // owned by another process.
  PipeClientIdentity selection_owner;
  uint64_t composition_sequence = 0;
  uint64_t ui_sequence = 0;
  uint64_t revision = 0;
  // Sequence of the latest Shift release that actually changed ascii_mode.
  // It remains on later revisions so the renderer can wait for a fresh caret
  // without mistaking focus/session changes for a user toggle.
  uint64_t mode_switch_sequence = 0;
  std::shared_ptr<const RuntimeStyleState> style;
};

bool IsShiftModeSwitch(const KeyEvent &key, uint32_t before_status,
                       uint32_t after_status) noexcept;

class RuntimeSnapshotSink {
public:
  virtual ~RuntimeSnapshotSink() = default;
  virtual void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept = 0;
  virtual bool
  PrepareStyle(std::string_view, bool,
               std::shared_ptr<const void> *presentation) noexcept {
    if (!presentation)
      return false;
    presentation->reset();
    return true;
  }
  virtual void
  ActivateStyle(std::shared_ptr<const RuntimeStyleState>) noexcept {}
  virtual void PrepareForRuntimeReady() noexcept {}
};

class RuntimeService {
public:
  RuntimeService() = default;
  ~RuntimeService();
  RuntimeService(const RuntimeService &) = delete;
  RuntimeService &operator=(const RuntimeService &) = delete;

  bool Start(const wchar_t *engine_path, const char *data_root,
             std::string *error);
  ControlError InitializeControlState(uint32_t empty_root_behavior_flags = 0);
  void SetSnapshotSink(RuntimeSnapshotSink *sink);
  void Stop() noexcept;
  Frame Dispatch(const Frame &request);
  // Runtime pipe entry point. Delivery-tracked commands are prepared without
  // touching the engine and can be executed/claimed exactly once across a
  // reconnect. In-process callers that do not cross a fallible transport keep
  // using Dispatch.
  Frame DispatchForDelivery(const Frame &request,
                            const PipeClientIdentity &owner);
  ControlError ExecuteControl(Command command);
  // Flip a boolean engine option on every live session and republish, so the
  // tray reflects the new status_flags now instead of on the next keystroke.
  // Also updates the in-memory overlay so sessions opened after the click
  // inherit the runtime toggle; it does not rewrite the config file.
  bool SetOption(std::string_view name, bool value);
  // Applies a related group (for example both traditionalization aliases) as
  // one transaction across every live context.
  bool SetOptions(const std::map<std::string, bool> &changes);
  // Status UI path: atomically persists a safe schema id and rolls the file
  // back if the engine cannot commit the matching context replacement.
  ControlError SelectSchemaAndPersist(std::string_view schema);
  RuntimeReadiness readiness() const noexcept;
  uint64_t engine_generation() const noexcept;
  void InvalidateConnection(uint64_t client_id, uint64_t activation_generation,
                            uint64_t connection_generation,
                            const PipeClientIdentity &owner);

private:
  friend struct RuntimeServiceTestAccess;
  friend class RuntimeControlService;

  static constexpr size_t kMaxDeliveriesPerClient = 8;
  static constexpr size_t kMaxDeliveries = 256;
  static constexpr size_t kMaxAcknowledgementsPerClient = 16;
  static constexpr size_t kMaxAcknowledgements = 512;
  static constexpr size_t kMaxClientsPerOwner = kRuntimeClientCapacity;
  static constexpr size_t kMaxClients = kRuntimeClientCapacity;
  static constexpr size_t kMaxSessionsPerClient = 32;
  static constexpr size_t kMaxSessionsPerOwner = 64;
  static constexpr size_t kMaxSessions = 256;
  static constexpr size_t kMaxRetiredContexts = 256;
  static constexpr size_t kMaxAbandonedEpochsPerOwner = 32;
  static constexpr size_t kMaxAbandonedEpochs = 512;
  static constexpr size_t kMaxAbandonedSessionsPerOwner = 64;
  static constexpr size_t kMaxAbandonedSessions = 512;

  struct SessionKey {
    uint64_t client_id;
    uint64_t activation_generation;
    uint64_t connection_generation;
    uint64_t session_id;
    uint64_t session_generation;
    auto operator<=>(const SessionKey &) const = default;
  };
  struct UiSessionState {
    PipeClientIdentity owner;
    std::atomic<std::shared_ptr<const RuntimeSnapshot>> latest;
  };
  struct Session {
    FamoEngineContext *context = nullptr;
    uint64_t last_sequence = 0;
    Correlation correlation;
    Composition composition;
    uint64_t composition_sequence = 0;
    uint64_t mode_switch_sequence = 0;
    std::shared_ptr<UiSessionState> ui;
    Correlation open_correlation;
    std::string open_schema;
    // A RESYNC_REQUIRED receipt proves this exact business sequence already
    // ran. Delivery Claim may only issue RECOVER until its final snapshot is
    // cached; no later business request may cross this state.
    uint32_t pending_recovery_action = 0;
    bool pending_recovery_handled = false;
    uint64_t pending_recovery_sequence = 0;
    bool pending_recovery_key_available = false;
    KeyEvent pending_recovery_key;
    uint32_t pending_recovery_status_before_key = 0;
    // If RECOVER produced the final engine-owned snapshot but host-side copy
    // or encoding could not finish, retain the lease for a Claim retry rather
    // than issuing RECOVER again or dropping the commit.
    FamoEngineActionResultLease pending_recovery_result;
  };
  struct ClientEpoch {
    uint64_t activation_generation = 0;
    uint64_t connection_generation = 0;
    PipeClientIdentity owner;
    uint64_t max_session_id = 0;
  };
  struct AbandonedEpoch {
    uint64_t client_id = 0;
    uint64_t activation_generation = 0;
    uint64_t connection_generation = 0;
    PipeClientIdentity owner;
  };
  struct AbandonedSession {
    DeliveryReference reference;
    PipeClientIdentity owner;
  };
  enum class DeliveryStage {
    Prepared,
    Executing,
    PendingRecovery,
    Completed,
    EncodingFailed,
    TerminalFailed
  };
  struct DeliveryEntry {
    DeliveryReference reference;
    Frame request;
    Frame final_reply;
    PipeClientIdentity owner;
    DeliveryStage stage = DeliveryStage::Prepared;
    uint32_t recovery_attempts = 0;
  };
  struct AcknowledgedDelivery {
    DeliveryReference reference;
    PipeClientIdentity owner;
  };

  Frame Reply(const Frame &request, Status status) const;
  Frame PrepareDeliveryLocked(const Frame &request,
                              const PipeClientIdentity &owner);
  Frame ExecutePreparedLocked(const Frame &request,
                              const DeliveryReference &reference,
                              const PipeClientIdentity &owner);
  Frame AdvanceDeliveryLocked(DeliveryEntry &delivery);
  Frame RetryCachedDeliveryEncodingLocked(DeliveryEntry &delivery);
  Frame ClaimDeliveryLocked(const Frame &request,
                            const DeliveryReference &reference,
                            const PipeClientIdentity &owner);
  Frame AcknowledgeDeliveryLocked(const Frame &request,
                                  const DeliveryReference &reference,
                                  const PipeClientIdentity &owner);
  Frame AbandonConnectionLocked(const Frame &request,
                                const PipeClientIdentity &owner);
  Frame AbandonSessionLocked(const Frame &request,
                             const DeliveryReference &reference,
                             const PipeClientIdentity &owner);
  bool IsAbandonedEpochLocked(const Correlation &correlation,
                              const PipeClientIdentity *owner = nullptr) const;
  bool RememberAbandonedEpochLocked(
      const Correlation &correlation, const PipeClientIdentity &owner);
  const AbandonedSession *FindAbandonedSessionLocked(
      const Correlation &correlation) const;
  bool RememberAbandonedSessionLocked(
      const DeliveryReference &reference, const PipeClientIdentity &owner);
  bool EnsureRetiredCapacityLocked(size_t additional);
  bool DestroyOrRetireContextLocked(FamoEngineContext *context);
  void RetryRetiredContextsLocked();
  void CleanupDeadOwnersLocked();
  bool ValidateDeliveryRequestLocked(const Frame &request) const;
  bool HasOutstandingDeliveryLocked(const SessionKey &key) const;
  bool HasOutstandingConnectionLocked(
      uint64_t client_id, uint64_t activation_generation,
      uint64_t connection_generation) const;
  bool RememberAcknowledgedDeliveryLocked(
      const DeliveryReference &reference, const PipeClientIdentity &owner);
  void CleanupDeadDeliveriesLocked();
  void CleanupDeliverySessionLocked(const DeliveryReference &reference);
  Frame DispatchUiState(const Frame &request,
                        const PipeClientIdentity *owner = nullptr);
  Frame DispatchLocked(const Frame &request,
                       const PipeClientIdentity *owner = nullptr);
  Frame DispatchSessionCommand(const Frame &request, const SessionKey &key,
                               Session &session);
  Frame RecoverSessionCommand(const Frame &request, const SessionKey &key,
                              Session &session);
  Frame CompleteSessionCommand(const Frame &request, Session &session,
                               uint32_t expected_action,
                               FamoEngineActionResultLease result);
  bool CopyResult(const FamoEngineActionResultV2 &result,
                  uint32_t expected_action, uint32_t max_candidates,
                  Composition *composition,
                  std::string *error) const;
  bool ReadStatusLocked(FamoEngineContext *context,
                        Composition *composition);
  void Publish(const Session &session, bool visible) noexcept;
  bool ApplyOptionsLocked(FamoEngineContext *context,
                          const std::map<std::string, bool> &options);
  bool ReplaceContextsLocked(std::string_view schema,
                             const std::map<std::string, bool> &options);
  ControlError ReloadStyle();
  ControlError ReloadOptions();
  ControlError SelectSchema();
  ControlError ResetUserDictionary();
  bool RecoverPendingUserDictionaryTransactions(std::string_view data_root);

  std::timed_mutex mutex_;
  std::mutex ui_sessions_mutex_;
  FamoEngineHost engine_;
  std::map<uint64_t, ClientEpoch> clients_;
  std::vector<AbandonedEpoch> abandoned_epochs_;
  std::vector<AbandonedSession> abandoned_sessions_;
  std::map<SessionKey, Session> sessions_;
  std::map<SessionKey, std::shared_ptr<UiSessionState>> ui_sessions_;
  // Replacement commits before old contexts retire. A transient destroy
  // failure therefore cannot make a live session point at a half-retired
  // context; teardown retries these before unloading the engine.
  std::vector<FamoEngineContext *> retired_contexts_;
  std::vector<DeliveryEntry> deliveries_;
  std::vector<AcknowledgedDelivery> acknowledged_deliveries_;
  std::map<std::string, bool> options_;
  std::string selected_schema_;
  std::wstring engine_path_;
  std::string data_root_;
  std::shared_ptr<const RuntimeStyleState> style_state_;
  std::atomic<RuntimeSnapshotSink *> snapshot_sink_{nullptr};
  bool started_ = false;
  std::atomic<RuntimeReadiness> readiness_{RuntimeReadiness::Unavailable};
  std::atomic<uint64_t> engine_generation_{0};
  std::atomic<uint64_t> snapshot_revision_{0};
};

} // namespace famo::runtime
