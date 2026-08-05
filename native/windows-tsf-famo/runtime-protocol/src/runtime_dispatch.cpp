#include "famo_runtime_service.h"

#include <algorithm>
#include <utility>

#include "runtime_style_config.h"

namespace famo::runtime {

bool IsShiftModeSwitch(const KeyEvent &key, uint32_t before_status,
                       uint32_t after_status) noexcept {
  constexpr uint32_t kRimeShiftLeft = 0xffe1;
  constexpr uint32_t kRimeShiftRight = 0xffe2;
  const bool shift =
      key.virtual_key == kRimeShiftLeft || key.virtual_key == kRimeShiftRight;
  return shift && key.is_key_down == 0 &&
         ((before_status ^ after_status) & FAMO_STATUS_ASCII_MODE) != 0;
}

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

Frame RuntimeService::DispatchLocked(const Frame &request,
                                     const PipeClientIdentity *owner) {
  if (!started_ || request.flags != 0 || request.status != Status::Ok)
    return Reply(request, Status::InvalidFrame);
  const auto &c = request.correlation;
  if (c.client_id == 0 || c.activation_generation == 0 ||
      c.connection_generation == 0)
    return Reply(request, Status::InvalidFrame);
  if (request.command == Command::Hello) {
    NegotiatedHello negotiated;
    std::string negotiation_error;
    if (c.session_id != 0 || c.session_generation != 0 || c.sequence != 0 ||
        !NegotiateHello(request, &negotiated, &negotiation_error))
      return Reply(request, Status::InvalidFrame);
    CleanupDeadOwnersLocked();
    if (IsAbandonedEpochLocked(c))
      return Reply(request, Status::StaleRequest);
    const auto found = clients_.find(c.client_id);
    if (found != clients_.end() && owner && found->second.owner &&
        found->second.owner != *owner) {
      return Reply(request, Status::StaleRequest);
    }
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
    PipeClientIdentity bound_owner =
        found == clients_.end() ? PipeClientIdentity{} : found->second.owner;
    if (owner && !bound_owner)
      bound_owner = *owner;
    if (advances) {
      if (found != clients_.end() &&
          HasOutstandingConnectionLocked(
              c.client_id, found->second.activation_generation,
              found->second.connection_generation)) {
        return Reply(request, Status::Unavailable);
      }
      if (found == clients_.end()) {
        const PipeClientIdentity capacity_owner =
            owner ? *owner : bound_owner;
        const size_t owner_clients =
            capacity_owner
                ? std::count_if(
                      clients_.begin(), clients_.end(),
                      [&](const auto &entry) {
                        return entry.second.owner == capacity_owner;
                      })
                : 0;
        if (clients_.size() >= kMaxClients ||
            owner_clients >= kMaxClientsPerOwner) {
          return Reply(request, Status::Unavailable);
        }
      }
      const size_t context_count =
          std::count_if(sessions_.begin(), sessions_.end(),
                        [&](const auto &entry) {
                          return entry.first.client_id == c.client_id;
                        });
      if (!EnsureRetiredCapacityLocked(context_count))
        return Reply(request, Status::Unavailable);
      std::lock_guard ui_lock(ui_sessions_mutex_);
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->first.client_id == c.client_id) {
          if (HasOutstandingDeliveryLocked(it->first)) {
            ++it;
            continue;
          }
          Publish(it->second, false);
          ui_sessions_.erase(it->first);
          (void)DestroyOrRetireContextLocked(it->second.context);
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
      if (found == clients_.end()) {
        try {
          clients_.emplace(
              c.client_id,
              ClientEpoch{c.activation_generation,
                          c.connection_generation, bound_owner, 0,
                          negotiated.protocol_version,
                          negotiated.bridge_abi});
        } catch (...) {
          return Reply(request, Status::Unavailable);
        }
      } else {
        found->second = ClientEpoch{
            c.activation_generation, c.connection_generation, bound_owner, 0,
            negotiated.protocol_version, negotiated.bridge_abi};
      }
    } else if (found != clients_.end() && owner && !found->second.owner) {
      found->second.owner = *owner;
    }
    if (found != clients_.end() &&
        (found->second.protocol_version != negotiated.protocol_version ||
         found->second.bridge_abi != negotiated.bridge_abi)) {
      return Reply(request, Status::StaleRequest);
    }
    Frame reply = Reply(request, Status::Ok);
    reply.wire_version = negotiated.protocol_version;
    reply.payload = std::move(negotiated.response_payload);
    return reply;
  }
  if (request.command == Command::OpenSession)
    CleanupDeadOwnersLocked();
  const auto client = clients_.find(c.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != c.activation_generation ||
      client->second.connection_generation != c.connection_generation ||
      (owner && client->second.owner != *owner)) {
    return Reply(request, Status::StaleRequest);
  }
  if (request.wire_version != client->second.protocol_version)
    return Reply(request, Status::InvalidFrame);
  if (request.command == Command::SearchCandidates) {
    if (client->second.protocol_version < 4 || c.session_id != 0 ||
        c.session_generation != 0 || c.sequence == 0)
      return Reply(request, Status::InvalidFrame);
    return DispatchSearchCandidatesLocked(request);
  }
  if (request.command == Command::GetStyleOverlay) {
    if (client->second.protocol_version < 5 || c.session_id != 0 ||
        c.session_generation != 0 || c.sequence == 0)
      return Reply(request, Status::InvalidFrame);
    return DispatchStyleOverlayLocked(request);
  }
  if (c.session_id == 0 || c.session_generation == 0 || c.sequence == 0)
    return Reply(request, Status::InvalidFrame);

  const SessionKey key{c.client_id, c.activation_generation,
                       c.connection_generation, c.session_id,
                       c.session_generation};
  if (FindAbandonedSessionLocked(c))
    return Reply(request, Status::StaleRequest);
  if (request.command == Command::OpenSession) {
    std::string schema, error;
    if (!DecodeOpenSession(request.payload, &schema, &error))
      return Reply(request, Status::InvalidFrame);
    const auto existing = sessions_.find(key);
    if (existing != sessions_.end()) {
      if (c != existing->second.open_correlation)
        return Reply(request, Status::StaleRequest);
      return Reply(request, schema == existing->second.open_schema
                                ? Status::Ok
                                : Status::InvalidFrame);
    }
    if (c.session_id <= client->second.max_session_id)
      return Reply(request, Status::StaleRequest);
    const size_t client_sessions =
        std::count_if(sessions_.begin(), sessions_.end(),
                      [&](const auto &entry) {
                        return entry.first.client_id == c.client_id;
                      });
    const size_t owner_sessions =
        client->second.owner
            ? std::count_if(
                  sessions_.begin(), sessions_.end(),
                  [&](const auto &entry) {
                    const auto session_client =
                        clients_.find(entry.first.client_id);
                    return session_client != clients_.end() &&
                           session_client->second.owner ==
                               client->second.owner;
                  })
            : 0;
    if (sessions_.size() >= kMaxSessions ||
        client_sessions >= kMaxSessionsPerClient ||
        owner_sessions >= kMaxSessionsPerOwner ||
        !EnsureRetiredCapacityLocked(1)) {
      return Reply(request, Status::Unavailable);
    }
    FamoEngineContext *context = nullptr;
    const std::string &effective_schema =
        selected_schema_.empty() ? schema : selected_schema_;
    const FamoUtf8String engine_schema = EngineString(effective_schema);
    if (engine_.CreateContext(&engine_schema, &context) !=
            FAMO_ENGINE_OK ||
        !context || !ApplyOptionsLocked(context, options_)) {
      if (context)
        (void)DestroyOrRetireContextLocked(context);
      return Reply(request, Status::EngineError);
    }
    Composition composition;
    if (!ReadStatusLocked(context, &composition)) {
      (void)DestroyOrRetireContextLocked(context);
      return Reply(request, Status::EngineError);
    }
    std::shared_ptr<UiSessionState> ui;
    try {
      ui = std::make_shared<UiSessionState>();
      ui->owner = client->second.owner;
      auto snapshot = std::make_shared<RuntimeSnapshot>();
      snapshot->correlation = c;
      snapshot->selection_owner = client->second.owner;
      snapshot->composition = composition;
      snapshot->composition_sequence = c.sequence;
      snapshot->style = style_state_;
      snapshot->revision = snapshot_revision_.fetch_add(1) + 1;
      ui->latest.store(std::move(snapshot));
      std::lock_guard ui_lock(ui_sessions_mutex_);
      ui_sessions_.emplace(key, ui);
      try {
        sessions_.emplace(
            key, Session{context, c.sequence, c, std::move(composition),
                         c.sequence, 0, ui, c, schema});
      } catch (...) {
        ui_sessions_.erase(key);
        throw;
      }
    } catch (...) {
      (void)DestroyOrRetireContextLocked(context);
      return Reply(request, Status::Unavailable);
    }
    client->second.max_session_id = c.session_id;
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

std::vector<std::string> RuntimeService::FilterSearchCandidates(
    std::span<const Candidate> candidates, std::string_view query) {
  std::vector<std::string> filtered;
  filtered.reserve((std::min)(candidates.size(),
                              static_cast<size_t>(kMaxSearchCandidateCount)));
  for (size_t index = 0; index < candidates.size(); ++index) {
    const std::string &text = candidates[index].text;
    if (text.empty() || text == query)
      continue;
    const bool duplicate = std::any_of(
        candidates.begin(), candidates.begin() + index,
        [&](const Candidate &earlier) { return earlier.text == text; });
    if (duplicate)
      continue;
    const bool has_shorter_prefix = std::any_of(
        candidates.begin(), candidates.end(), [&](const Candidate &other) {
          return !other.text.empty() && other.text.size() < text.size() &&
                 text.starts_with(other.text);
        });
    if (has_shorter_prefix)
      continue;
    filtered.push_back(text);
    if (filtered.size() == kMaxSearchCandidateCount)
      break;
  }
  return filtered;
}

Frame RuntimeService::DispatchStyleOverlayLocked(const Frame &request) {
  // The overlay is read here because this process owns the user's data root
  // and is the only one that can reach it from every host, sandboxed or not.
  RuntimeStyleOverlay overlay;
  if (!ReadRuntimeStyleOverlay(data_root_, &overlay))
    return Reply(request, Status::Unavailable);
  std::string error;
  Frame reply = Reply(request, Status::Ok);
  if (!EncodeStyleOverlay(overlay.text, overlay.exists, &reply.payload,
                          &error)) {
    return Reply(request, Status::Unavailable);
  }
  return reply;
}

Frame RuntimeService::DispatchSearchCandidatesLocked(const Frame &request) {
  std::string query;
  std::string error;
  if (!DecodeSearchQuery(request.payload, &query, &error))
    return Reply(request, Status::InvalidFrame);

  for (char &character : query) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (value >= 'A' && value <= 'Z') {
      character = static_cast<char>(value - 'A' + 'a');
      continue;
    }
    if ((value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '\'') {
      continue;
    }
    Frame reply = Reply(request, Status::Ok);
    if (!EncodeSearchCandidates({}, &reply.payload, &error))
      return Reply(request, Status::Unavailable);
    return reply;
  }

  if (!EnsureRetiredCapacityLocked(1))
    return Reply(request, Status::Unavailable);
  FamoEngineContext *context = nullptr;
  const FamoUtf8String engine_schema = EngineString(selected_schema_);
  if (engine_.CreateContext(&engine_schema, &context) != FAMO_ENGINE_OK ||
      !context || !ApplyOptionsLocked(context, options_)) {
    if (context)
      (void)DestroyOrRetireContextLocked(context);
    return Reply(request, Status::EngineError);
  }

  Frame result = Reply(request, Status::EngineError);
  try {
    Composition composition;
    bool converted = true;
    uint64_t timestamp = 1;
    for (const unsigned char character : query) {
      FamoEngineActionRequestV2 action =
          FamoEngineHost::Action(FAMO_ENGINE_ACTION_PROCESS_KEY);
      action.key = EngineKey(
          {character, 0, 0, 1, timestamp++});
      FamoEngineActionResultLease action_result;
      FamoEngineRecoveryOutcome outcome;
      const int32_t action_status = engine_.ExecuteActionRecovering(
          context, &action, 2, &action_result, &outcome);
      Composition next_composition;
      if (!outcome.business_dispatched || outcome.recovery_pending ||
          action_status != FAMO_ENGINE_OK || !action_result ||
          action_result->action != action.action ||
          action_result->result_flags != 0 ||
          (action_result->handled != 0) != outcome.handled ||
          !CopyResult(*action_result, action.action,
                      FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES, &next_composition,
                      &error)) {
        converted = false;
        break;
      }
      composition = std::move(next_composition);
    }
    if (converted) {
      const std::vector<std::string> candidates =
          FilterSearchCandidates(composition.candidates, query);
      result = Reply(request, Status::Ok);
      if (!EncodeSearchCandidates(candidates, &result.payload, &error))
        result = Reply(request, Status::Unavailable);
    }
  } catch (...) {
    result = Reply(request, Status::Unavailable);
  }
  if (!DestroyOrRetireContextLocked(context) &&
      result.status == Status::Ok) {
    return Reply(request, Status::Unavailable);
  }
  return result;
}

Frame RuntimeService::DispatchSessionCommand(const Frame &request,
                                             const SessionKey &key,
                                             Session &session) {
  const Correlation &c = request.correlation;
  if (request.command == Command::CloseSession) {
    if (!request.payload.empty())
      return Reply(request, Status::InvalidFrame);
    if (HasOutstandingDeliveryLocked(key))
      return Reply(request, Status::Unavailable);
    if (!EnsureRetiredCapacityLocked(1))
      return Reply(request, Status::Unavailable);
    {
      std::lock_guard ui_lock(ui_sessions_mutex_);
      ui_sessions_.erase(key);
      Publish(session, false);
    }
    if (!DestroyOrRetireContextLocked(session.context))
      return Reply(request, Status::Unavailable);
    sessions_.erase(key);
    return Reply(request, Status::Ok);
  }
  if (session.pending_recovery_action != 0 ||
      session.pending_recovery_result) {
    return Reply(request, Status::Unavailable);
  }

  std::string error;
  KeyEvent processed_key;
  bool processed_key_available = false;
  FamoEngineActionRequestV2 action{};
  const uint32_t status_before_key = session.composition.status_flags;
  if (request.command == Command::ProcessKey) {
    KeyEvent value;
    if (!DecodeKeyEvent(request.payload, &value, &error))
      return Reply(request, Status::InvalidFrame);
    processed_key = value;
    processed_key_available = true;
    action = FamoEngineHost::Action(FAMO_ENGINE_ACTION_PROCESS_KEY);
    action.key = EngineKey(value);
  } else if (request.command == Command::SelectCandidate ||
             request.command == Command::SelectCandidateAbsolute) {
    uint32_t index = 0;
    if (request.command == Command::SelectCandidateAbsolute) {
      uint64_t expected_sequence = 0;
      if (!DecodeAbsoluteCandidateSelection(
              request.payload, &index, &expected_sequence, &error))
        return Reply(request, Status::InvalidFrame);
      const uint64_t preview_start =
          (static_cast<uint64_t>(session.composition.page_index) + 1) *
          session.composition.page_size;
      const uint64_t preview_end =
          preview_start + session.composition.preview_candidates.size();
      if (expected_sequence != session.composition_sequence)
        return Reply(request, Status::StaleRequest);
      if (preview_start > UINT32_MAX || index < preview_start ||
          index >= preview_end)
        return Reply(request, Status::InvalidFrame);
      action = FamoEngineHost::Action(
          FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE);
    } else {
      if (!DecodeCandidateIndex(request.payload, &index, &error))
        return Reply(request, Status::InvalidFrame);
      action =
          FamoEngineHost::Action(FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
    }
    action.index = index;
  } else if (request.command == Command::CommitComposition ||
             request.command == Command::ClearComposition) {
    if (!request.payload.empty())
      return Reply(request, Status::InvalidFrame);
    action = FamoEngineHost::Action(
        request.command == Command::CommitComposition
            ? FAMO_ENGINE_ACTION_COMMIT_COMPOSITION
            : FAMO_ENGINE_ACTION_CLEAR_COMPOSITION);
  } else if (request.command == Command::HighlightCandidate) {
    uint32_t index = 0;
    if (!DecodeCandidateIndex(request.payload, &index, &error))
      return Reply(request, Status::InvalidFrame);
    action =
        FamoEngineHost::Action(FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE);
    action.index = index;
  } else if (request.command == Command::ChangePage) {
    bool backward = false;
    if (!DecodePageDirection(request.payload, &backward, &error))
      return Reply(request, Status::InvalidFrame);
    action = FamoEngineHost::Action(FAMO_ENGINE_ACTION_CHANGE_PAGE);
    action.value = backward ? 1 : 0;
  } else {
    return Reply(request, Status::InvalidFrame);
  }

  FamoEngineActionResultLease action_result;
  FamoEngineRecoveryOutcome outcome;
  const int32_t action_status = engine_.ExecuteActionRecovering(
      session.context, &action, 1, &action_result, &outcome);
  if (!outcome.business_dispatched) {
    return Reply(request, Status::EngineError);
  }
  session.last_sequence = c.sequence;
  session.pending_recovery_action = action.action;
  session.pending_recovery_handled = outcome.handled;
  session.pending_recovery_sequence = c.sequence;
  session.pending_recovery_key_available = processed_key_available;
  session.pending_recovery_key = processed_key;
  session.pending_recovery_status_before_key = status_before_key;
  if (outcome.recovery_pending)
    return Reply(request, Status::RecoveryPending);
  if (action_status != FAMO_ENGINE_OK || !action_result ||
      action_result->action != action.action ||
      action_result->result_flags != 0 ||
      (action_result->handled != 0) != outcome.handled) {
    return Reply(request, Status::RecoveryPending);
  }
  return CompleteSessionCommand(request, session, action.action,
                                std::move(action_result));
}

Frame RuntimeService::RecoverSessionCommand(const Frame &request,
                                            const SessionKey &key,
                                            Session &session) {
  (void)key;
  const Correlation &c = request.correlation;
  if (session.pending_recovery_action == 0 ||
      session.pending_recovery_sequence != c.sequence ||
      session.last_sequence != c.sequence || !session.context) {
    return Reply(request, Status::StaleRequest);
  }

  FamoEngineActionResultLease action_result;
  if (session.pending_recovery_result) {
    action_result = std::move(session.pending_recovery_result);
  } else {
    FamoEngineActionRequestV2 recovery =
        FamoEngineHost::Action(FAMO_ENGINE_ACTION_RECOVER);
    recovery.value =
        static_cast<int32_t>(session.pending_recovery_action);
    if (engine_.ExecuteAction(session.context, &recovery, &action_result) !=
            FAMO_ENGINE_OK ||
        !action_result) {
      return Reply(request, Status::RecoveryPending);
    }
  }
  if (action_result->action != session.pending_recovery_action ||
      action_result->result_flags != 0 ||
      (action_result->handled != 0) != session.pending_recovery_handled) {
    session.pending_recovery_result = std::move(action_result);
    return Reply(request, Status::RecoveryPending);
  }
  return CompleteSessionCommand(request, session,
                                session.pending_recovery_action,
                                std::move(action_result));
}

Frame RuntimeService::CompleteSessionCommand(
    const Frame &request, Session &session, uint32_t expected_action,
    FamoEngineActionResultLease action_result) {
  try {
  const Correlation &c = request.correlation;
  std::string error;
  Composition composition;
  if (!action_result ||
      !CopyResult(*action_result, expected_action,
                  FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES, &composition, &error)) {
    session.pending_recovery_result = std::move(action_result);
    return Reply(request, Status::RecoveryPending);
  }

  const FamoUtf8String vertical_option{sizeof(FamoUtf8String), "_vertical", 9};
  int32_t rime_vertical = 0;
  if (engine_.GetOption(session.context, &vertical_option, &rime_vertical) ==
          FAMO_ENGINE_OK &&
      rime_vertical != 0)
    composition.state_flags |= kHostRimeVertical;

  // macOS parity: optional, read-only preview of the following one or two
  // candidate pages. The engine iterator does not move the live page/highlight;
  // any unsupported/error path simply leaves the preview empty.
  if ((composition.state_flags & kHostPreviewPages) != 0 &&
      composition.is_last_page == 0 && composition.page_size > 0) {
    const uint32_t rows =
        (composition.state_flags & kHostPreviewRowsTwo) != 0 ? 2u : 1u;
    const uint64_t start =
        (static_cast<uint64_t>(composition.page_index) + 1) *
        composition.page_size;
    const uint32_t count = static_cast<uint32_t>((std::min)(
        static_cast<uint64_t>((std::min)(
            kMaxCandidateCount, FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES)),
        static_cast<uint64_t>(composition.page_size) * rows));
    if (start <= UINT32_MAX && count > 0) {
      FamoEngineActionRequestV2 preview_action =
          FamoEngineHost::Action(FAMO_ENGINE_ACTION_PEEK_CANDIDATES);
      preview_action.index = static_cast<uint32_t>(start);
      preview_action.count = count;
      FamoEngineActionResultLease preview_result;
      if (engine_.ExecuteAction(session.context, &preview_action,
                                &preview_result) == FAMO_ENGINE_OK &&
          preview_result) {
        Composition preview;
        std::string preview_error;
        if (CopyResult(*preview_result, preview_action.action,
                       preview_action.count, &preview, &preview_error))
          composition.preview_candidates = std::move(preview.candidates);
      }
    }
  }
  Frame reply = Reply(request, Status::Ok);
  bool encoded = false;
  try {
    encoded = EncodeComposition(composition, &reply.payload, &error);
  } catch (...) {
    encoded = false;
  }
  if (!encoded) {
    // Preview/candidate UI is optional; the handled decision and exact commit
    // are not.  A primary ABI result fits the protocol budget by construction,
    // but an additional preview result may not.  Shed only optional rendering
    // data and retry without copying the commit.
    Composition essential;
    essential.handled = composition.handled;
    essential.commit = std::move(composition.commit);
    essential.status_flags = composition.status_flags;
    essential.state_flags =
        composition.state_flags &
        ~(FAMO_COMPOSITION_HAS_PREEDIT |
          FAMO_COMPOSITION_HAS_CANDIDATES);
    if (essential.commit.empty())
      essential.state_flags &= ~FAMO_COMPOSITION_HAS_COMMIT;
    else
      essential.state_flags |= FAMO_COMPOSITION_HAS_COMMIT;
    try {
      encoded = EncodeComposition(essential, &reply.payload, &error);
    } catch (...) {
      encoded = false;
    }
    if (encoded)
      composition = std::move(essential);
  }
  if (!encoded) {
    session.pending_recovery_result = std::move(action_result);
    return Reply(request, Status::RecoveryPending);
  }
  session.correlation = c;
  if (session.pending_recovery_key_available &&
      IsShiftModeSwitch(session.pending_recovery_key,
                        session.pending_recovery_status_before_key,
                        composition.status_flags))
    session.mode_switch_sequence = c.sequence;
  session.composition = std::move(composition);
  session.composition_sequence = c.sequence;
  session.pending_recovery_action = 0;
  session.pending_recovery_handled = false;
  session.pending_recovery_sequence = 0;
  session.pending_recovery_key_available = false;
  session.pending_recovery_key = {};
  session.pending_recovery_status_before_key = 0;
  session.pending_recovery_result.Reset();
  Publish(session, true);
  return reply;
  } catch (...) {
    // Once the engine has returned a final result it no longer owns a
    // recoverable snapshot. Keep that exact engine allocation alive across
    // every host-side allocation/formatting failure so Claim can retry without
    // replaying the business action or issuing a now-invalid RECOVER.
    session.pending_recovery_result = std::move(action_result);
    return Reply(request, Status::RecoveryPending);
  }
}

} // namespace famo::runtime
