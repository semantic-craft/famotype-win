#include "famo_runtime_service.h"

#include <chrono>
#include <cstddef>
#include <cstring>

namespace famo::runtime {
namespace {

bool CopyString(const FamoUtf8String &source, std::string *target,
                size_t *wire_size) {
  constexpr size_t kMaxPayload = kMaxFrameSize - kHeaderSize;
  const size_t field_size = 4u + static_cast<size_t>(source.length_bytes);
  if (!target || !wire_size || source.length_bytes > kMaxStringBytes ||
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

} // namespace

RuntimeService::~RuntimeService() { Stop(); }

bool RuntimeService::Start(const wchar_t *engine_path, const char *data_root,
                           std::string *error) {
  std::lock_guard lock(mutex_);
  if (started_)
    return true;
  readiness_.store(RuntimeReadiness::Starting);
  const int32_t rc = engine_.Load(engine_path, data_root);
  if (rc != FAMO_ENGINE_OK || !engine_.AbiRunnable()) {
    if (error)
      *error = rc != FAMO_ENGINE_OK
                   ? "engine load failed: " + std::to_string(rc)
                   : "engine does not expose the required v1.1 API";
    if (rc == FAMO_ENGINE_OK)
      engine_.Unload();
    readiness_.store(RuntimeReadiness::Unavailable);
    return false;
  }
  data_root_ = data_root ? data_root : "";
  engine_path_ = engine_path ? engine_path : L"";
  started_ = true;
  engine_generation_.store(1);
  return true;
}

void RuntimeService::SetSnapshotSink(RuntimeSnapshotSink *sink) {
  snapshot_sink_.store(sink);
}

void RuntimeService::Stop() {
  readiness_.store(RuntimeReadiness::Stopping);
  std::lock_guard lock(mutex_);
  std::lock_guard ui_lock(ui_sessions_mutex_);
  if (!started_) {
    ui_sessions_.clear();
    readiness_.store(RuntimeReadiness::Unavailable);
    return;
  }
  for (auto &[key, session] : sessions_) {
    (void)key;
    Publish(session, false);
    if (session.context)
      engine_.api().destroy_context(session.context);
  }
  sessions_.clear();
  ui_sessions_.clear();
  clients_.clear();
  options_.clear();
  selected_schema_.clear();
  engine_path_.clear();
  data_root_.clear();
  style_state_ = std::make_shared<const RuntimeStyleState>();
  engine_.Unload();
  started_ = false;
  readiness_.store(RuntimeReadiness::Unavailable);
}

Frame RuntimeService::Reply(const Frame &request, Status status) const {
  Frame reply;
  reply.command = request.command;
  reply.flags = kFlagResponse;
  reply.status = status;
  reply.correlation = request.correlation;
  return reply;
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

Frame RuntimeService::DispatchUiState(const Frame &request) {
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
    if (found == ui_sessions_.end())
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

bool RuntimeService::CopyView(const FamoCompositionView &view,
                              Composition *target, std::string *error) const {
  constexpr size_t kV12End =
      offsetof(FamoCompositionView, is_last_page) + sizeof(uint32_t);
  constexpr size_t kFixedBytes = 4 + 4 + (9 * 4);
  size_t wire_size =
      kFixedBytes + (static_cast<size_t>(view.candidate_count) * 8);
  if (!target || view.size < kV12End ||
      view.candidate_count > kMaxCandidateCount ||
      (view.candidate_count > 0 && !view.candidates) ||
      !CopyString(view.preedit, &target->preedit, &wire_size) ||
      !CopyString(view.commit, &target->commit, &wire_size) ||
      !CopyString(view.commit_preview, &target->commit_preview, &wire_size) ||
      !CopyString(view.schema_id, &target->schema_id, &wire_size) ||
      !CopyString(view.schema_name, &target->schema_name, &wire_size)) {
    if (error)
      *error = "invalid engine composition view";
    return false;
  }
  target->handled = (view.state_flags & FAMO_COMPOSITION_HANDLED) != 0;
  target->highlighted_index = view.highlighted_index;
  target->page_index = view.page_index;
  target->page_size = view.page_size;
  target->state_flags = view.state_flags;
  if (style_state_)
    target->state_flags |= style_state_->host_behavior_flags;
  target->preedit_sel_start = view.preedit_sel_start;
  target->preedit_sel_end = view.preedit_sel_end;
  target->preedit_cursor_pos = view.preedit_cursor_pos;
  target->status_flags = view.status_flags;
  target->is_last_page = view.is_last_page;
  target->candidates.reserve(view.candidate_count);
  for (uint32_t i = 0; i < view.candidate_count; ++i) {
    const FamoCandidate &source = view.candidates[i];
    Candidate candidate;
    if (source.size < sizeof(FamoCandidate) ||
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
}

bool RuntimeService::ReadStatusLocked(FamoEngineContext *context,
                                      Composition *target) {
  if (!context || !target)
    return false;
  FamoCompositionView view{};
  view.size = static_cast<uint32_t>(sizeof(view));
  if (engine_.api().get_status(context, &view) != FAMO_ENGINE_OK)
    return false;
  Composition composition;
  std::string error;
  const bool copied = CopyView(view, &composition, &error);
  const int32_t free_rc = engine_.FreeView(&view);
  if (!copied || free_rc != FAMO_ENGINE_OK)
    return false;
  *target = std::move(composition);
  return true;
}

void RuntimeService::InvalidateConnection(uint64_t client_id,
                                          uint64_t activation_generation,
                                          uint64_t connection_generation) {
  std::lock_guard lock(mutex_);
  std::lock_guard ui_lock(ui_sessions_mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->first.client_id == client_id &&
        it->first.activation_generation == activation_generation &&
        it->first.connection_generation == connection_generation) {
      Publish(it->second, false);
      ui_sessions_.erase(it->first);
      if (it->second.context)
        engine_.api().destroy_context(it->second.context);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace famo::runtime
