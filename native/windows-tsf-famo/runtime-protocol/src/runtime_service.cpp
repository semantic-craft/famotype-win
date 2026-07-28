#include "famo_runtime_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace famo::runtime {
namespace {

static_assert(kMaxStringBytes == FAMO_ENGINE_V2_MAX_STRING_BYTES);
static_assert(kMaxEngineResultBytes == FAMO_ENGINE_V2_MAX_RESULT_BYTES);

bool CopyString(const FamoUtf8String &source, std::string *target,
                size_t *wire_size) {
  constexpr size_t kMaxPayload = kMaxFramePayloadSize;
  const size_t field_size = 4u + static_cast<size_t>(source.length_bytes);
  if (!target || !wire_size ||
      source.size < FAMO_UTF8_STRING_REQUIRED_SIZE ||
      source.length_bytes > kMaxStringBytes ||
      *wire_size > kMaxPayload || field_size > kMaxPayload - *wire_size ||
      (source.length_bytes > 0 && !source.data)) {
    return false;
  }
  const std::string_view value(source.data ? source.data : "",
                               source.length_bytes);
  if (!IsValidUtf8(value))
    return false;
  *wire_size += field_size;
  target->assign(value);
  return true;
}

bool IsUtf8Boundary(std::string_view value, uint32_t offset) {
  if (offset > value.size())
    return false;
  return offset == value.size() ||
         (static_cast<unsigned char>(value[offset]) & 0xc0u) != 0x80u;
}

} // namespace

RuntimeService::~RuntimeService() { Stop(); }

bool RuntimeService::Start(const wchar_t *engine_path, const char *data_root,
                           std::string *error) {
  std::lock_guard lock(mutex_);
  if (started_)
    return true;
  readiness_.store(RuntimeReadiness::Starting);
  std::wstring next_engine_path;
  std::string next_data_root;
  const auto fail = [&](std::string_view message) {
    try {
      engine_.Unload();
    } catch (...) {
    }
    engine_path_.clear();
    data_root_.clear();
    started_ = false;
    readiness_.store(RuntimeReadiness::Unavailable);
    try {
      if (error)
        error->assign(message);
    } catch (...) {
    }
    return false;
  };
  try {
    deliveries_.reserve(kMaxDeliveries);
    acknowledged_deliveries_.reserve(kMaxAcknowledgements);
    abandoned_epochs_.reserve(kMaxAbandonedEpochs);
    abandoned_sessions_.reserve(kMaxAbandonedSessions);
    retired_contexts_.reserve(kMaxRetiredContexts);
    next_engine_path.assign(engine_path ? engine_path : L"");
    next_data_root.assign(data_root ? data_root : "");
    wchar_t allocation_failure[2]{};
    if (GetEnvironmentVariableW(
            L"FAMO_TEST_RUNTIME_START_PARAMETER_ALLOCATION_FAILURE",
            allocation_failure,
            static_cast<DWORD>(std::size(allocation_failure))) > 0) {
      throw std::bad_alloc();
    }
  } catch (...) {
    return fail("runtime start state allocation failed");
  }
  if (next_data_root.size() > kMaxStringBytes ||
      !IsValidUtf8(next_data_root)) {
    return fail("runtime data root is not valid bounded UTF-8");
  }
  try {
    if (!RecoverPendingUserDictionaryTransactions(next_data_root))
      return fail("pending user dictionary recovery failed");
  } catch (...) {
    return fail("pending user dictionary recovery failed");
  }
  int32_t rc = FAMO_ENGINE_E_RUNTIME;
  try {
    rc = engine_.LoadV2(next_engine_path.c_str(), next_data_root.c_str());
  } catch (...) {
    return fail("engine load raised an exception");
  }
  if (rc != FAMO_ENGINE_OK || !engine_.V2Runnable()) {
    if (rc != FAMO_ENGINE_OK) {
      try {
        return fail("engine load failed: " + std::to_string(rc));
      } catch (...) {
        return fail("engine load failed");
      }
    }
    return fail("engine does not expose the required v2 API");
  }
  data_root_.swap(next_data_root);
  engine_path_.swap(next_engine_path);
  started_ = true;
  engine_generation_.store(1);
  return true;
}

void RuntimeService::SetSnapshotSink(RuntimeSnapshotSink *sink) {
  snapshot_sink_.store(sink);
}

void RuntimeService::Stop() noexcept {
  try {
  readiness_.store(RuntimeReadiness::Stopping);
  std::lock_guard lock(mutex_);
  std::lock_guard ui_lock(ui_sessions_mutex_);
  deliveries_.clear();
  acknowledged_deliveries_.clear();
  abandoned_epochs_.clear();
  abandoned_sessions_.clear();
  if (!started_) {
    sessions_.clear();
    ui_sessions_.clear();
    clients_.clear();
    retired_contexts_.clear();
    readiness_.store(RuntimeReadiness::Unavailable);
    return;
  }
  for (auto &[key, session] : sessions_) {
    (void)key;
    Publish(session, false);
    if (session.context)
      engine_.DestroyContext(session.context);
  }
  for (FamoEngineContext *context : retired_contexts_) {
    if (context)
      engine_.DestroyContext(context);
  }
  sessions_.clear();
  retired_contexts_.clear();
  ui_sessions_.clear();
  clients_.clear();
  options_.clear();
  selected_schema_.clear();
  engine_path_.clear();
  data_root_.clear();
  style_state_.reset();
  engine_.Unload();
  started_ = false;
  readiness_.store(RuntimeReadiness::Unavailable);
  } catch (...) {
    // Destruction is a process boundary: teardown may degrade or leak an
    // already-failing engine resource, but it must never terminate Runtime.
    style_state_.reset();
    started_ = false;
    readiness_.store(RuntimeReadiness::Unavailable);
    try {
      engine_.Unload();
    } catch (...) {
    }
  }
}

Frame RuntimeService::Reply(const Frame &request, Status status) const {
  Frame reply;
  reply.command = request.command;
  reply.flags = kFlagResponse;
  reply.status = status;
  reply.correlation = request.correlation;
  return reply;
}

bool RuntimeService::IsAbandonedEpochLocked(
    const Correlation &correlation,
    const PipeClientIdentity *owner) const {
  return std::any_of(
      abandoned_epochs_.begin(), abandoned_epochs_.end(),
      [&](const AbandonedEpoch &entry) {
        return entry.client_id == correlation.client_id &&
               entry.activation_generation ==
                   correlation.activation_generation &&
               entry.connection_generation ==
                   correlation.connection_generation &&
               (!owner || entry.owner == *owner);
      });
}

bool RuntimeService::RememberAbandonedEpochLocked(
    const Correlation &correlation, const PipeClientIdentity &owner) {
  const auto existing = std::find_if(
      abandoned_epochs_.begin(), abandoned_epochs_.end(),
      [&](const AbandonedEpoch &entry) {
        return entry.client_id == correlation.client_id &&
               entry.activation_generation ==
                   correlation.activation_generation &&
               entry.connection_generation ==
                   correlation.connection_generation;
      });
  if (existing != abandoned_epochs_.end())
    return existing->owner == owner;

  const size_t owner_count =
      std::count_if(abandoned_epochs_.begin(), abandoned_epochs_.end(),
                    [&](const AbandonedEpoch &entry) {
                      return entry.owner == owner;
                    });
  if (owner_count >= kMaxAbandonedEpochsPerOwner) {
    const auto oldest = std::find_if(
        abandoned_epochs_.begin(), abandoned_epochs_.end(),
        [&](const AbandonedEpoch &entry) {
          return entry.owner == owner;
        });
    if (oldest != abandoned_epochs_.end())
      abandoned_epochs_.erase(oldest);
  }
  if (abandoned_epochs_.size() >= kMaxAbandonedEpochs)
    abandoned_epochs_.erase(abandoned_epochs_.begin());
  try {
    abandoned_epochs_.push_back(
        {correlation.client_id, correlation.activation_generation,
         correlation.connection_generation, owner});
  } catch (...) {
    return false;
  }
  return true;
}

const RuntimeService::AbandonedSession *
RuntimeService::FindAbandonedSessionLocked(
    const Correlation &correlation) const {
  const auto found = std::find_if(
      abandoned_sessions_.begin(), abandoned_sessions_.end(),
      [&](const AbandonedSession &entry) {
        const Correlation &known = entry.reference.correlation;
        return known.client_id == correlation.client_id &&
               known.activation_generation ==
                   correlation.activation_generation &&
               known.connection_generation ==
                   correlation.connection_generation &&
               known.session_id == correlation.session_id &&
               known.session_generation == correlation.session_generation;
      });
  return found == abandoned_sessions_.end() ? nullptr : &*found;
}

bool RuntimeService::RememberAbandonedSessionLocked(
    const DeliveryReference &reference, const PipeClientIdentity &owner) {
  if (const AbandonedSession *existing =
          FindAbandonedSessionLocked(reference.correlation)) {
    return existing->owner == owner && existing->reference == reference;
  }

  const size_t owner_count =
      std::count_if(abandoned_sessions_.begin(), abandoned_sessions_.end(),
                    [&](const AbandonedSession &entry) {
                      return entry.owner == owner;
                    });
  if (owner_count >= kMaxAbandonedSessionsPerOwner) {
    const auto oldest = std::find_if(
        abandoned_sessions_.begin(), abandoned_sessions_.end(),
        [&](const AbandonedSession &entry) {
          return entry.owner == owner;
        });
    if (oldest != abandoned_sessions_.end())
      abandoned_sessions_.erase(oldest);
  }
  if (abandoned_sessions_.size() >= kMaxAbandonedSessions)
    abandoned_sessions_.erase(abandoned_sessions_.begin());
  try {
    abandoned_sessions_.push_back({reference, owner});
  } catch (...) {
    return false;
  }
  return true;
}

void RuntimeService::RetryRetiredContextsLocked() {
  std::erase_if(retired_contexts_, [&](FamoEngineContext *context) {
    return !context ||
           engine_.DestroyContext(context) == FAMO_ENGINE_OK;
  });
}

bool RuntimeService::EnsureRetiredCapacityLocked(size_t additional) {
  RetryRetiredContextsLocked();
  if (retired_contexts_.size() > kMaxRetiredContexts ||
      additional > kMaxRetiredContexts - retired_contexts_.size())
    return false;
  try {
    retired_contexts_.reserve(retired_contexts_.size() + additional);
  } catch (...) {
    return false;
  }
  return true;
}

bool RuntimeService::DestroyOrRetireContextLocked(
    FamoEngineContext *context) {
  if (!context || engine_.DestroyContext(context) == FAMO_ENGINE_OK)
    return true;
  if (retired_contexts_.size() >= kMaxRetiredContexts)
    return false;
  try {
    retired_contexts_.push_back(context);
  } catch (...) {
    return false;
  }
  return true;
}

void RuntimeService::CleanupDeadOwnersLocked() {
  RetryRetiredContextsLocked();
  std::erase_if(abandoned_epochs_, [](const AbandonedEpoch &entry) {
    return !PipeClientIsAlive(entry.owner);
  });
  std::erase_if(abandoned_sessions_, [](const AbandonedSession &entry) {
    return !PipeClientIsAlive(entry.owner);
  });
  for (auto client = clients_.begin(); client != clients_.end();) {
    if (!client->second.owner ||
        PipeClientIsAlive(client->second.owner)) {
      ++client;
      continue;
    }
    const uint64_t client_id = client->first;
    const size_t context_count =
        std::count_if(sessions_.begin(), sessions_.end(),
                      [&](const auto &entry) {
                        return entry.first.client_id == client_id &&
                               entry.first.activation_generation ==
                                   client->second.activation_generation &&
                               entry.first.connection_generation ==
                                   client->second.connection_generation;
                      });
    if (!EnsureRetiredCapacityLocked(context_count)) {
      ++client;
      continue;
    }

    std::erase_if(deliveries_, [&](const DeliveryEntry &entry) {
      return entry.reference.correlation.client_id == client_id &&
             entry.owner == client->second.owner;
    });
    std::erase_if(
        acknowledged_deliveries_,
        [&](const AcknowledgedDelivery &entry) {
          return entry.reference.correlation.client_id == client_id &&
                 entry.owner == client->second.owner;
        });
    std::erase_if(abandoned_epochs_, [&](const AbandonedEpoch &entry) {
      return entry.owner == client->second.owner;
    });
    std::erase_if(abandoned_sessions_,
                  [&](const AbandonedSession &entry) {
                    return entry.owner == client->second.owner;
                  });
    {
      std::lock_guard ui_lock(ui_sessions_mutex_);
      for (auto session = sessions_.begin(); session != sessions_.end();) {
        if (session->first.client_id == client_id &&
            session->first.activation_generation ==
                client->second.activation_generation &&
            session->first.connection_generation ==
                client->second.connection_generation) {
          Publish(session->second, false);
          ui_sessions_.erase(session->first);
          (void)DestroyOrRetireContextLocked(session->second.context);
          session = sessions_.erase(session);
        } else {
          ++session;
        }
      }
    }
    client = clients_.erase(client);
  }
}

Frame RuntimeService::Dispatch(const Frame &request) {
  if (readiness_.load() != RuntimeReadiness::Ready)
    return Reply(request, Status::Unavailable);
  if (request.command == Command::UpdateUiState)
    return DispatchUiState(request);
  std::unique_lock<std::timed_mutex> lock(mutex_, std::try_to_lock);
  if (!lock && !lock.try_lock_for(std::chrono::milliseconds(1)))
    return Reply(request, Status::Unavailable);
  if (!lock || readiness_.load() != RuntimeReadiness::Ready)
    return Reply(request, Status::Unavailable);
  return DispatchLocked(request);
}

Frame RuntimeService::DispatchUiState(const Frame &request,
                                      const PipeClientIdentity *owner) {
  if (request.flags != 0 || request.status != Status::Ok)
    return Reply(request, Status::InvalidFrame);
  const Correlation &c = request.correlation;
  if (c.client_id == 0 || c.activation_generation == 0 ||
      c.connection_generation == 0 || c.session_id == 0 ||
      c.session_generation == 0 || c.sequence == 0)
    return Reply(request, Status::InvalidFrame);

  UiState state;
  std::string error;
  if (!DecodeUiState(request.payload, &state, &error))
    return Reply(request, Status::InvalidFrame);
  const SessionKey key{c.client_id, c.activation_generation,
                       c.connection_generation, c.session_id,
                       c.session_generation};
  std::shared_ptr<const RuntimeSnapshot> next;
  {
    std::lock_guard lock(ui_sessions_mutex_);
    const auto found = ui_sessions_.find(key);
    if (found == ui_sessions_.end() ||
        (owner && found->second->owner != *owner))
      return Reply(request, Status::StaleRequest);
    std::shared_ptr<const RuntimeSnapshot> current =
        found->second->latest.load();
    for (;;) {
      if (!current || c.sequence <= current->ui_sequence ||
          c.sequence <= current->composition_sequence)
        return Reply(request, Status::StaleRequest);
      try {
        auto candidate = std::make_shared<RuntimeSnapshot>(*current);
        candidate->correlation = c;
        candidate->ui_state = state;
        candidate->ui_sequence = c.sequence;
        candidate->revision = snapshot_revision_.fetch_add(1) + 1;
        next = std::move(candidate);
      } catch (...) {
        return Reply(request, Status::Unavailable);
      }
      if (found->second->latest.compare_exchange_weak(current, next))
        break;
    }
  }
  if (RuntimeSnapshotSink *sink = snapshot_sink_.load())
    sink->Publish(next);
  return Reply(request, Status::Ok);
}

RuntimeReadiness RuntimeService::readiness() const noexcept {
  return readiness_.load();
}

uint64_t RuntimeService::engine_generation() const noexcept {
  return engine_generation_.load();
}

void RuntimeService::Publish(const Session &session, bool visible) noexcept {
  if (!session.ui)
    return;
  try {
    std::shared_ptr<const RuntimeSnapshot> current = session.ui->latest.load();
    std::shared_ptr<const RuntimeSnapshot> next;
    for (;;) {
      auto snapshot = current ? std::make_shared<RuntimeSnapshot>(*current)
                              : std::make_shared<RuntimeSnapshot>();
      snapshot->correlation = session.correlation;
      snapshot->selection_owner = session.ui->owner;
      snapshot->composition = session.composition;
      snapshot->composition_sequence = session.composition_sequence;
      snapshot->style = style_state_;
      snapshot->revision = snapshot_revision_.fetch_add(1) + 1;
      snapshot->mode_switch_sequence =
          visible ? session.mode_switch_sequence : uint64_t{0};
      if (!visible) {
        snapshot->composition = {};
        snapshot->ui_state.focused = false;
        snapshot->ui_state.layout_available = false;
      }
      next = std::move(snapshot);
      if (session.ui->latest.compare_exchange_weak(current, next))
        break;
    }
    if (RuntimeSnapshotSink *sink = snapshot_sink_.load())
      sink->Publish(std::move(next));
  } catch (...) {
    // UI publication is best effort and must never change an engine reply.
  }
}

bool RuntimeService::CopyResult(const FamoEngineActionResultV2 &result,
                                uint32_t expected_action,
                                uint32_t max_candidates, Composition *target,
                                std::string *error) const {
  try {
  char copy_failure[2]{};
  if (GetEnvironmentVariableA("FAMO_TEST_RUNTIME_COPY_FAILURE", copy_failure,
                              static_cast<DWORD>(std::size(copy_failure))) >
      0) {
    if (error)
      *error = "injected runtime copy failure";
    return false;
  }
  if (!target ||
      !FamoEngineHost::ValidateResultV2(
          &result, expected_action, max_candidates, kMaxStringBytes)) {
    if (error)
      *error = "invalid engine action result";
    return false;
  }
  const FamoCompositionViewV2 &view = result.view;
  constexpr uint32_t kEngineContentFlags =
      FAMO_COMPOSITION_HAS_PREEDIT | FAMO_COMPOSITION_HAS_COMMIT |
      FAMO_COMPOSITION_HAS_CANDIDATES;
  constexpr uint32_t kEngineStatusFlags =
      FAMO_STATUS_ASCII_MODE | FAMO_STATUS_COMPOSING | FAMO_STATUS_DISABLED |
      FAMO_STATUS_FULL_SHAPE | FAMO_STATUS_ASCII_PUNCT |
      FAMO_STATUS_SIMPLIFIED;
  constexpr size_t kFixedBytes = 4 + 4 + (9 * 4);
  const uint32_t wire_candidate_count =
      (std::min)(view.candidate_count, kMaxCandidateCount);
  size_t wire_size =
      kFixedBytes + (static_cast<size_t>(wire_candidate_count) * 8);
  if (result.struct_size < FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE ||
      result.action != expected_action || result.result_flags != 0 ||
      result.handled > 1 ||
      ((expected_action == FAMO_ENGINE_ACTION_STATUS ||
        expected_action == FAMO_ENGINE_ACTION_PEEK_CANDIDATES) &&
       result.handled != 0) ||
      view.struct_size < FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE ||
      view.layout_version != FAMO_COMPOSITION_LAYOUT_V2 ||
      view.candidate_layout_version != FAMO_CANDIDATE_LAYOUT_V2 ||
      view.candidate_stride != FAMO_CANDIDATE_V2_STRIDE ||
      (view.state_flags & ~kEngineContentFlags) != 0 ||
      (view.status_flags & ~kEngineStatusFlags) != 0 ||
      view.candidate_count > max_candidates ||
      (view.candidate_count > 0 && !view.candidates) ||
      (view.candidate_count > 0 &&
       reinterpret_cast<uintptr_t>(view.candidates) %
               alignof(FamoCandidateV2) !=
           0) ||
      !CopyString(view.preedit, &target->preedit, &wire_size) ||
      !CopyString(view.commit, &target->commit, &wire_size) ||
      !CopyString(view.commit_preview, &target->commit_preview, &wire_size) ||
      !CopyString(view.schema_id, &target->schema_id, &wire_size) ||
      !CopyString(view.schema_name, &target->schema_name, &wire_size)) {
    if (error)
      *error = "invalid engine composition view";
    return false;
  }
  const auto has_content = [&view](uint32_t flag) {
    return (view.state_flags & flag) != 0;
  };
  if (view.is_last_page > 1 ||
      (view.candidate_count == 0 && view.highlighted_index != 0) ||
      (view.candidate_count == 0 &&
       (view.page_index != 0 || view.page_size != 0)) ||
      (view.candidate_count > 0 &&
       (view.highlighted_index >= view.candidate_count ||
        view.page_size == 0)) ||
      has_content(FAMO_COMPOSITION_HAS_PREEDIT) !=
          !target->preedit.empty() ||
      has_content(FAMO_COMPOSITION_HAS_COMMIT) != !target->commit.empty() ||
      has_content(FAMO_COMPOSITION_HAS_CANDIDATES) !=
          (view.candidate_count > 0) ||
      ((expected_action == FAMO_ENGINE_ACTION_STATUS ||
        expected_action == FAMO_ENGINE_ACTION_PEEK_CANDIDATES) &&
       !target->commit.empty()) ||
      view.preedit_sel_start > view.preedit_sel_end ||
      !IsUtf8Boundary(target->preedit, view.preedit_sel_start) ||
      !IsUtf8Boundary(target->preedit, view.preedit_sel_end) ||
      !IsUtf8Boundary(target->preedit, view.preedit_cursor_pos)) {
    if (error)
      *error = "inconsistent engine composition view";
    return false;
  }
  target->handled = result.handled != 0;
  target->highlighted_index =
      wire_candidate_count == 0
          ? 0
          : (std::min)(view.highlighted_index,
                       wire_candidate_count - 1);
  target->page_index = view.page_index;
  target->page_size = view.page_size;
  target->state_flags = view.state_flags;
  if (target->handled)
    target->state_flags |= FAMO_COMPOSITION_HANDLED;
  if (style_state_)
    target->state_flags |= style_state_->host_behavior_flags;
  target->preedit_sel_start = view.preedit_sel_start;
  target->preedit_sel_end = view.preedit_sel_end;
  target->preedit_cursor_pos = view.preedit_cursor_pos;
  target->status_flags = view.status_flags;
  target->is_last_page = view.is_last_page;
  target->candidates.reserve(wire_candidate_count);
  const auto *candidate_bytes =
      reinterpret_cast<const unsigned char *>(view.candidates);
  for (uint32_t i = 0; i < wire_candidate_count; ++i) {
    const auto &source = *reinterpret_cast<const FamoCandidateV2 *>(
        candidate_bytes + static_cast<size_t>(i) * view.candidate_stride);
    Candidate candidate;
    if (source.struct_size < FAMO_CANDIDATE_V2_STRIDE ||
        (source.flags & ~FAMO_CANDIDATE_FLAG_DEFAULT) != 0 ||
        !CopyString(source.text, &candidate.text, &wire_size) ||
        !CopyString(source.comment, &candidate.comment, &wire_size) ||
        !CopyString(source.label, &candidate.label, &wire_size)) {
      if (error)
        *error = "invalid engine candidate";
      return false;
    }
    candidate.quality = source.quality;
    candidate.flags = source.flags;
    target->candidates.push_back(std::move(candidate));
  }
  return true;
  } catch (...) {
    try {
      if (error)
        *error = "engine composition allocation failed";
    } catch (...) {
    }
    return false;
  }
}

bool RuntimeService::ReadStatusLocked(FamoEngineContext *context,
                                      Composition *target) {
  if (!context || !target)
    return false;
  FamoEngineActionRequestV2 request =
      FamoEngineHost::Action(FAMO_ENGINE_ACTION_STATUS);
  FamoEngineActionResultLease result;
  if (engine_.ExecuteAction(context, &request, &result) != FAMO_ENGINE_OK ||
      !result)
    return false;
  Composition composition;
  std::string error;
  const bool copied =
      CopyResult(*result, request.action,
                 FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES, &composition, &error);
  if (!copied)
    return false;
  *target = std::move(composition);
  return true;
}

void RuntimeService::InvalidateConnection(uint64_t client_id,
                                          uint64_t activation_generation,
                                          uint64_t connection_generation,
                                          const PipeClientIdentity &owner) {
  std::lock_guard lock(mutex_);
  const auto client = clients_.find(client_id);
  if (!owner || client == clients_.end() ||
      client->second.activation_generation != activation_generation ||
      client->second.connection_generation != connection_generation ||
      client->second.owner != owner) {
    return;
  }
  // A physical pipe can fail after one session's business mutation. Recovery
  // reconnects the same logical epoch, so retain every sibling session in that
  // epoch; otherwise unrelated focused documents would become stale.
  if (HasOutstandingConnectionLocked(client_id, activation_generation,
                                     connection_generation)) {
    return;
  }
  const size_t context_count =
      std::count_if(sessions_.begin(), sessions_.end(),
                    [&](const auto &entry) {
                      return entry.first.client_id == client_id &&
                             entry.first.activation_generation ==
                                 activation_generation &&
                             entry.first.connection_generation ==
                                 connection_generation;
                    });
  if (!EnsureRetiredCapacityLocked(context_count))
    return;
  std::lock_guard ui_lock(ui_sessions_mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->first.client_id == client_id &&
        it->first.activation_generation == activation_generation &&
        it->first.connection_generation == connection_generation) {
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
  clients_.erase(client);
}

} // namespace famo::runtime
