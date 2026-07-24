#include "famo_runtime_service.h"

#include <utility>

namespace famo::runtime {
namespace {

FamoUtf8String EngineString(const std::string &value) {
  return {static_cast<uint32_t>(sizeof(FamoUtf8String)), value.data(),
          static_cast<uint32_t>(value.size())};
}

FamoKeyEvent EngineKey(const KeyEvent &value) {
  FamoKeyEvent key{};
  key.size = static_cast<uint32_t>(sizeof(key));
  key.virtual_key = value.virtual_key;
  key.scan_code = value.scan_code;
  key.modifiers = value.modifiers;
  key.is_key_down = value.is_key_down;
  key.timestamp_ms = value.timestamp_ms;
  return key;
}

} // namespace

Frame RuntimeService::DispatchLocked(const Frame &request) {
  if (!started_ || request.flags != 0 || request.status != Status::Ok)
    return Reply(request, Status::InvalidFrame);
  const auto &c = request.correlation;
  if (c.client_id == 0 || c.activation_generation == 0 ||
      c.connection_generation == 0)
    return Reply(request, Status::InvalidFrame);
  if (request.command == Command::Hello) {
    if (!request.payload.empty() || c.session_id != 0 ||
        c.session_generation != 0 || c.sequence != 0)
      return Reply(request, Status::InvalidFrame);
    const auto found = clients_.find(c.client_id);
    if (found != clients_.end() &&
        (c.activation_generation < found->second.activation_generation ||
         (c.activation_generation == found->second.activation_generation &&
          c.connection_generation < found->second.connection_generation))) {
      return Reply(request, Status::StaleRequest);
    }
    const bool advances =
        found == clients_.end() ||
        c.activation_generation > found->second.activation_generation ||
        c.connection_generation > found->second.connection_generation;
    if (advances) {
      std::lock_guard ui_lock(ui_sessions_mutex_);
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->first.client_id == c.client_id) {
          Publish(it->second, false);
          ui_sessions_.erase(it->first);
          engine_.api().destroy_context(it->second.context);
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
      clients_[c.client_id] =
          ClientEpoch{c.activation_generation, c.connection_generation};
    }
    return Reply(request, Status::Ok);
  }
  const auto client = clients_.find(c.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != c.activation_generation ||
      client->second.connection_generation != c.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  if (c.session_id == 0 || c.session_generation == 0 || c.sequence == 0)
    return Reply(request, Status::InvalidFrame);

  const SessionKey key{c.client_id, c.activation_generation,
                       c.connection_generation, c.session_id,
                       c.session_generation};
  if (request.command == Command::OpenSession) {
    if (sessions_.contains(key))
      return Reply(request, Status::StaleRequest);
    std::string schema, error;
    if (!DecodeOpenSession(request.payload, &schema, &error))
      return Reply(request, Status::InvalidFrame);
    FamoEngineContext *context = nullptr;
    const std::string &effective_schema =
        selected_schema_.empty() ? schema : selected_schema_;
    const FamoUtf8String engine_schema = EngineString(effective_schema);
    if (engine_.api().create_context(&engine_schema, &context) !=
            FAMO_ENGINE_OK ||
        !context || !ApplyOptionsLocked(context, options_)) {
      if (context)
        engine_.api().destroy_context(context);
      return Reply(request, Status::EngineError);
    }
    auto ui = std::make_shared<UiSessionState>();
    auto snapshot = std::make_shared<RuntimeSnapshot>();
    snapshot->correlation = c;
    snapshot->style = style_state_;
    snapshot->revision = snapshot_revision_.fetch_add(1) + 1;
    ui->latest.store(std::move(snapshot));
    sessions_.emplace(key,
                      Session{context, c.sequence, c, Composition{}, 0, ui});
    {
      std::lock_guard ui_lock(ui_sessions_mutex_);
      ui_sessions_[key] = std::move(ui);
    }
    return Reply(request, Status::Ok);
  }

  const auto found = sessions_.find(key);
  if (found == sessions_.end())
    return Reply(request, Status::StaleRequest);
  if (c.sequence <= found->second.last_sequence) {
    return Reply(request, Status::StaleRequest);
  }
  return DispatchSessionCommand(request, key, found->second);
}

Frame RuntimeService::DispatchSessionCommand(const Frame &request,
                                             const SessionKey &key,
                                             Session &session) {
  const Correlation &c = request.correlation;
  if (request.command != Command::UpdateUiState)
    session.last_sequence = c.sequence;
  if (request.command == Command::CloseSession) {
    if (!request.payload.empty())
      return Reply(request, Status::InvalidFrame);
    {
      std::lock_guard ui_lock(ui_sessions_mutex_);
      ui_sessions_.erase(key);
      Publish(session, false);
    }
    const int32_t rc = engine_.api().destroy_context(session.context);
    sessions_.erase(key);
    return Reply(request,
                 rc == FAMO_ENGINE_OK ? Status::Ok : Status::EngineError);
  }

  FamoCompositionView view{};
  int32_t rc = FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::string error;
  std::string explicit_commit;
  Composition composition;
  bool composition_ready = false;
  if (request.command == Command::ProcessKey) {
    KeyEvent value;
    if (!DecodeKeyEvent(request.payload, &value, &error))
      return Reply(request, Status::InvalidFrame);
    const FamoKeyEvent engine_key = EngineKey(value);
    rc = engine_.api().process_key(session.context, &engine_key, &view);
  } else if (request.command == Command::SelectCandidate) {
    uint32_t index = 0;
    if (!DecodeCandidateIndex(request.payload, &index, &error))
      return Reply(request, Status::InvalidFrame);
    rc = engine_.api().select_candidate(session.context, index, &view);
    if (rc == FAMO_ENGINE_OK) {
      composition_ready = CopyView(view, &composition, &error);
      const int32_t free_rc = engine_.FreeView(&view);
      if (!composition_ready || free_rc != FAMO_ENGINE_OK)
        return Reply(request, Status::EngineError);
    }
    if (rc == FAMO_ENGINE_OK && composition.commit.empty()) {
      // The Rime ABI intentionally leaves the selection commit pending so the
      // legacy Weasel host can retrieve it through its simulated VK_SELECT.
      // Reproduce that established response step inside the new runtime.
      constexpr uint32_t kRimeSelectKeysym = 0xff60;
      const FamoKeyEvent select_key{sizeof(FamoKeyEvent), kRimeSelectKeysym,
                                    0, 0, 1, 0};
      composition = {};
      composition_ready = false;
      rc = engine_.api().process_key(session.context, &select_key, &view);
    }
  } else if (request.command == Command::CommitComposition ||
             request.command == Command::ClearComposition) {
    if (!request.payload.empty())
      return Reply(request, Status::InvalidFrame);
    if (request.command == Command::CommitComposition)
      explicit_commit = session.composition.commit_preview;
    rc = request.command == Command::CommitComposition
             ? engine_.api().commit_composition(session.context)
             : engine_.api().clear_composition(session.context);
    if (rc == FAMO_ENGINE_OK)
      rc = engine_.api().get_status(session.context, &view);
  } else if (request.command == Command::HighlightCandidate) {
    uint32_t index = 0;
    if (!DecodeCandidateIndex(request.payload, &index, &error))
      return Reply(request, Status::InvalidFrame);
    rc = engine_.api().highlight_candidate(session.context, index);
    if (rc == FAMO_ENGINE_OK)
      rc = engine_.api().get_status(session.context, &view);
  } else if (request.command == Command::ChangePage) {
    bool backward = false;
    if (!DecodePageDirection(request.payload, &backward, &error))
      return Reply(request, Status::InvalidFrame);
    rc = engine_.api().change_page(session.context, backward ? 1 : 0);
    if (rc == FAMO_ENGINE_OK)
      rc = engine_.api().get_status(session.context, &view);
  } else {
    return Reply(request, Status::InvalidFrame);
  }
  if (rc != FAMO_ENGINE_OK)
    return Reply(request, Status::EngineError);

  if (!composition_ready) {
    const bool copied = CopyView(view, &composition, &error);
    const int32_t free_rc = engine_.FreeView(&view);
    if (!copied || free_rc != FAMO_ENGINE_OK)
      return Reply(request, Status::EngineError);
  }
  // The v1.1 commit_composition ABI returns only success; the following
  // get_status deliberately does not consume or repeat a commit. Preserve the
  // already-validated preview from the last engine view so the host can apply
  // the exact text that librime committed.
  if (request.command == Command::CommitComposition &&
      !explicit_commit.empty()) {
    composition.commit = std::move(explicit_commit);
    composition.handled = true;
    composition.state_flags |=
        FAMO_COMPOSITION_HAS_COMMIT | FAMO_COMPOSITION_HANDLED;
  }
  Frame reply = Reply(request, Status::Ok);
  if (!EncodeComposition(composition, &reply.payload, &error))
    return Reply(request, Status::EngineError);
  session.correlation = c;
  session.composition = std::move(composition);
  session.composition_sequence = c.sequence;
  Publish(session, true);
  return reply;
}

} // namespace famo::runtime
