#pragma once

#include <atomic>
#include <compare>
#include <map>
#include <memory>
#include <mutex>

#include "../../engine-api/host/famo_engine_host.h"
#include "famo_runtime_protocol.h"

namespace famo::runtime {

struct RuntimeStyleState {
  uint32_t host_behavior_flags = 0;
  std::shared_ptr<const void> presentation;
};

struct RuntimeSnapshot {
  Correlation correlation;
  Composition composition;
  UiState ui_state;
  uint64_t composition_sequence = 0;
  uint64_t ui_sequence = 0;
  uint64_t revision = 0;
  std::shared_ptr<const RuntimeStyleState> style;
};

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
  void Stop();
  Frame Dispatch(const Frame &request);
  ControlError ExecuteControl(Command command);
  // Flip a boolean engine option on every live session and republish, so the
  // tray reflects the new status_flags now instead of on the next keystroke.
  // Also updates the in-memory overlay so sessions opened after the click
  // inherit the runtime toggle; it does not rewrite the config file.
  bool SetOption(std::string_view name, bool value);
  RuntimeReadiness readiness() const noexcept;
  uint64_t engine_generation() const noexcept;
  void InvalidateConnection(uint64_t client_id, uint64_t activation_generation,
                            uint64_t connection_generation);

private:
  struct SessionKey {
    uint64_t client_id;
    uint64_t activation_generation;
    uint64_t connection_generation;
    uint64_t session_id;
    uint64_t session_generation;
    auto operator<=>(const SessionKey &) const = default;
  };
  struct UiSessionState {
    std::atomic<std::shared_ptr<const RuntimeSnapshot>> latest;
  };
  struct Session {
    FamoEngineContext *context = nullptr;
    uint64_t last_sequence = 0;
    Correlation correlation;
    Composition composition;
    uint64_t composition_sequence = 0;
    std::shared_ptr<UiSessionState> ui;
  };
  struct ClientEpoch {
    uint64_t activation_generation = 0;
    uint64_t connection_generation = 0;
  };

  Frame Reply(const Frame &request, Status status) const;
  Frame DispatchUiState(const Frame &request);
  Frame DispatchLocked(const Frame &request);
  Frame DispatchSessionCommand(const Frame &request, const SessionKey &key,
                               Session &session);
  bool CopyView(const FamoCompositionView &view, Composition *composition,
                std::string *error) const;
  void Publish(const Session &session, bool visible) noexcept;
  bool ApplyOptionsLocked(FamoEngineContext *context,
                          const std::map<std::string, bool> &options) const;
  bool ReplaceContextsLocked(std::string_view schema,
                             const std::map<std::string, bool> &options);
  ControlError ReloadStyle();
  ControlError ReloadOptions();
  ControlError SelectSchema();
  ControlError ResetUserDictionary();

  std::timed_mutex mutex_;
  std::mutex ui_sessions_mutex_;
  FamoEngineHost engine_;
  std::map<uint64_t, ClientEpoch> clients_;
  std::map<SessionKey, Session> sessions_;
  std::map<SessionKey, std::shared_ptr<UiSessionState>> ui_sessions_;
  std::map<std::string, bool> options_;
  std::string selected_schema_;
  std::wstring engine_path_;
  std::string data_root_;
  std::shared_ptr<const RuntimeStyleState> style_state_ =
      std::make_shared<const RuntimeStyleState>();
  std::atomic<RuntimeSnapshotSink *> snapshot_sink_{nullptr};
  bool started_ = false;
  std::atomic<RuntimeReadiness> readiness_{RuntimeReadiness::Unavailable};
  std::atomic<uint64_t> engine_generation_{0};
  std::atomic<uint64_t> snapshot_revision_{0};
};

} // namespace famo::runtime
