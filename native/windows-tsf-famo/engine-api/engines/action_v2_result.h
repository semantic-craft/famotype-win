#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../famo_engine_api.h"

namespace famo_action_v2 {

struct CandidateSnapshot {
  std::string text;
  std::string comment;
  std::string label;
  uint32_t quality = 0;
  uint32_t flags = 0;
};

struct Snapshot {
  std::string preedit;
  std::string commit;
  std::vector<CandidateSnapshot> candidates;
  uint32_t highlighted_index = 0;
  uint32_t page_index = 0;
  uint32_t page_size = 0;
  uint32_t state_flags = 0;
  uint32_t preedit_sel_start = 0;
  uint32_t preedit_sel_end = 0;
  uint32_t preedit_cursor_pos = 0;
  std::string commit_preview;
  std::string schema_id;
  std::string schema_name;
  uint32_t status_flags = 0;
  uint32_t is_last_page = 0;
};

static_assert(std::is_nothrow_move_constructible<Snapshot>::value,
              "recovery must retain a completed snapshot without allocating");
static_assert(std::is_nothrow_move_assignable<Snapshot>::value,
              "recovery must retain a completed snapshot without allocating");

inline bool AddSize(size_t value, size_t* total) {
  if (!total || value > (std::numeric_limits<size_t>::max)() - *total)
    return false;
  *total += value;
  return true;
}

inline bool AddStringSize(const std::string& value, size_t* total) {
  return value.empty() ||
         (value.size() < (std::numeric_limits<size_t>::max)() &&
          AddSize(value.size() + 1, total));
}

inline size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

inline bool SnapshotAllocationSize(const Snapshot& snapshot,
                                   size_t* out_size) noexcept {
  if (!out_size ||
      snapshot.candidates.size() >
          FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES) {
    return false;
  }
  size_t total =
      AlignUp(sizeof(FamoEngineActionResultV2), alignof(FamoCandidateV2));
  const size_t stride = FAMO_CANDIDATE_V2_STRIDE;
  if (snapshot.candidates.size() >
          (std::numeric_limits<size_t>::max)() / stride ||
      !AddSize(snapshot.candidates.size() * stride, &total) ||
      !AddStringSize(snapshot.preedit, &total) ||
      !AddStringSize(snapshot.commit, &total) ||
      !AddStringSize(snapshot.commit_preview, &total) ||
      !AddStringSize(snapshot.schema_id, &total) ||
      !AddStringSize(snapshot.schema_name, &total)) {
    return false;
  }
  for (const auto& candidate : snapshot.candidates) {
    if (!AddStringSize(candidate.text, &total) ||
        !AddStringSize(candidate.comment, &total) ||
        !AddStringSize(candidate.label, &total)) {
      return false;
    }
  }
  *out_size = total;
  return true;
}

// Keep commit lossless and shed only reconstructable UI/status text until the
// one-allocation ABI contract fits. A later STATUS refreshes omitted UI.
inline bool TrimOptionalToResultBudget(Snapshot* snapshot) noexcept {
  if (!snapshot ||
      snapshot->commit.size() > FAMO_ENGINE_V2_MAX_STRING_BYTES) {
    return false;
  }
  if (snapshot->preedit.size() > FAMO_ENGINE_V2_MAX_STRING_BYTES) {
    snapshot->preedit.clear();
    snapshot->preedit_sel_start = 0;
    snapshot->preedit_sel_end = 0;
    snapshot->preedit_cursor_pos = 0;
    snapshot->state_flags &= ~FAMO_COMPOSITION_HAS_PREEDIT;
  }
  if (snapshot->commit_preview.size() >
      FAMO_ENGINE_V2_MAX_STRING_BYTES)
    snapshot->commit_preview.clear();
  if (snapshot->schema_id.size() > FAMO_ENGINE_V2_MAX_STRING_BYTES)
    snapshot->schema_id.clear();
  if (snapshot->schema_name.size() > FAMO_ENGINE_V2_MAX_STRING_BYTES)
    snapshot->schema_name.clear();
  bool candidates_fit =
      snapshot->candidates.size() <= FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES;
  for (const auto& candidate : snapshot->candidates) {
    candidates_fit =
        candidates_fit &&
        candidate.text.size() <= FAMO_ENGINE_V2_MAX_STRING_BYTES &&
        candidate.comment.size() <= FAMO_ENGINE_V2_MAX_STRING_BYTES &&
        candidate.label.size() <= FAMO_ENGINE_V2_MAX_STRING_BYTES;
  }
  if (!candidates_fit) {
    snapshot->candidates.clear();
    snapshot->highlighted_index = 0;
    snapshot->state_flags &= ~FAMO_COMPOSITION_HAS_CANDIDATES;
  }

  size_t total = 0;
  if (!SnapshotAllocationSize(*snapshot, &total))
    return false;
  if (total > FAMO_ENGINE_V2_MAX_RESULT_BYTES) {
    snapshot->candidates.clear();
    snapshot->highlighted_index = 0;
    snapshot->state_flags &= ~FAMO_COMPOSITION_HAS_CANDIDATES;
  }
  if (!SnapshotAllocationSize(*snapshot, &total))
    return false;
  if (total > FAMO_ENGINE_V2_MAX_RESULT_BYTES)
    snapshot->commit_preview.clear();
  if (!SnapshotAllocationSize(*snapshot, &total))
    return false;
  if (total > FAMO_ENGINE_V2_MAX_RESULT_BYTES) {
    snapshot->schema_name.clear();
    snapshot->schema_id.clear();
  }
  if (!SnapshotAllocationSize(*snapshot, &total))
    return false;
  if (total > FAMO_ENGINE_V2_MAX_RESULT_BYTES) {
    snapshot->preedit.clear();
    snapshot->preedit_sel_start = 0;
    snapshot->preedit_sel_end = 0;
    snapshot->preedit_cursor_pos = 0;
    snapshot->state_flags &= ~FAMO_COMPOSITION_HAS_PREEDIT;
  }
  if (snapshot->candidates.empty()) {
    snapshot->highlighted_index = 0;
    snapshot->page_index = 0;
    snapshot->page_size = 0;
    snapshot->is_last_page = 1;
    snapshot->state_flags &= ~FAMO_COMPOSITION_HAS_CANDIDATES;
  }
  return SnapshotAllocationSize(*snapshot, &total) &&
         total <= FAMO_ENGINE_V2_MAX_RESULT_BYTES;
}

inline bool ValidUtf8Bytes(const char* data, size_t length) noexcept {
  if (length != 0 && !data)
    return false;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  size_t index = 0;
  while (index < length) {
    const unsigned char first = bytes[index++];
    if (first <= 0x7f)
      continue;

    size_t trailing = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      trailing = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      trailing = 2;
      if (first == 0xe0)
        second_min = 0xa0;  // Reject overlong encodings.
      else if (first == 0xed)
        second_max = 0x9f;  // Reject UTF-16 surrogate code points.
    } else if (first >= 0xf0 && first <= 0xf4) {
      trailing = 3;
      if (first == 0xf0)
        second_min = 0x90;  // Reject overlong encodings.
      else if (first == 0xf4)
        second_max = 0x8f;  // Reject code points above U+10FFFF.
    } else {
      return false;
    }

    if (trailing > length - index)
      return false;
    const unsigned char second = bytes[index++];
    if (second < second_min || second > second_max)
      return false;
    for (size_t offset = 1; offset < trailing; ++offset) {
      const unsigned char continuation = bytes[index++];
      if (continuation < 0x80 || continuation > 0xbf)
        return false;
    }
  }
  return true;
}

inline bool ValidInputString(const FamoUtf8String* value,
                             bool allow_null = false) noexcept {
  if (!value)
    return allow_null;
  if (value->size < FAMO_UTF8_STRING_REQUIRED_SIZE ||
      value->length_bytes > FAMO_ENGINE_V2_MAX_STRING_BYTES ||
      (value->length_bytes != 0 && !value->data)) {
    return false;
  }
  return (value->length_bytes == 0 ||
          std::memchr(value->data, '\0', value->length_bytes) == nullptr) &&
         ValidUtf8Bytes(value->data, value->length_bytes);
}

// Owns every published v2 result for one engine DLL. A result is a single host
// allocation (result + strided candidates + strings). The live set is the
// private release cookie: free_result validates only the pointer identity and
// never reads caller-mutable public fields.
class ResultStore {
 public:
  bool Start() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }

  int32_t Publish(const FamoEngineHostApi& host,
                  const FamoEngineActionRequestV2& request, bool handled,
                  const Snapshot& snapshot,
                  FamoEngineActionResultV2** out_result,
                  uint32_t result_flags = 0) {
    if (!out_result || !host.alloc || !host.free)
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
    *out_result = nullptr;
    if ((result_flags & ~FAMO_ENGINE_RESULT_RESYNC_REQUIRED) != 0)
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
    if (snapshot.candidates.size() >
        FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES)
      return FAMO_ENGINE_E_RUNTIME;
    const auto string_fits = [](const std::string& value) {
      return value.size() <= FAMO_ENGINE_V2_MAX_STRING_BYTES;
    };
    if (!string_fits(snapshot.preedit) || !string_fits(snapshot.commit) ||
        !string_fits(snapshot.commit_preview) ||
        !string_fits(snapshot.schema_id) ||
        !string_fits(snapshot.schema_name)) {
      return FAMO_ENGINE_E_RUNTIME;
    }
    for (const auto& candidate : snapshot.candidates) {
      if (!string_fits(candidate.text) || !string_fits(candidate.comment) ||
          !string_fits(candidate.label)) {
        return FAMO_ENGINE_E_RUNTIME;
      }
    }

    const size_t stride = request.candidate_stride;
    const size_t candidate_offset =
        AlignUp(sizeof(FamoEngineActionResultV2), alignof(FamoCandidateV2));
    if (snapshot.candidates.size() >
        (std::numeric_limits<size_t>::max)() / stride) {
      return FAMO_ENGINE_E_RUNTIME;
    }
    size_t total = candidate_offset;
    if (!AddSize(snapshot.candidates.size() * stride, &total) ||
        !AddStringSize(snapshot.preedit, &total) ||
        !AddStringSize(snapshot.commit, &total) ||
        !AddStringSize(snapshot.commit_preview, &total) ||
        !AddStringSize(snapshot.schema_id, &total) ||
        !AddStringSize(snapshot.schema_name, &total)) {
      return FAMO_ENGINE_E_RUNTIME;
    }
    for (const auto& candidate : snapshot.candidates) {
      if (!AddStringSize(candidate.text, &total) ||
          !AddStringSize(candidate.comment, &total) ||
          !AddStringSize(candidate.label, &total)) {
        return FAMO_ENGINE_E_RUNTIME;
      }
    }
    if (total > FAMO_ENGINE_V2_MAX_RESULT_BYTES)
      return FAMO_ENGINE_E_RUNTIME;

    auto* allocation = static_cast<unsigned char*>(host.alloc(total));
    if (!allocation)
      return FAMO_ENGINE_E_RUNTIME;
    std::memset(allocation, 0, total);
    auto* result =
        reinterpret_cast<FamoEngineActionResultV2*>(allocation);
    result->struct_size = static_cast<uint32_t>(sizeof(*result));
    result->action = request.action;
    result->handled = handled ? 1u : 0u;
    result->result_flags = result_flags;
    FamoCompositionViewV2& view = result->view;
    view.struct_size = static_cast<uint32_t>(sizeof(view));
    view.layout_version = request.view_layout_version;
    view.candidate_layout_version = request.candidate_layout_version;
    view.candidate_stride = request.candidate_stride;
    view.candidate_count =
        static_cast<uint32_t>(snapshot.candidates.size());
    view.highlighted_index = snapshot.highlighted_index;
    view.page_index = snapshot.page_index;
    view.page_size = snapshot.page_size;
    view.state_flags = snapshot.state_flags;
    view.preedit_sel_start = snapshot.preedit_sel_start;
    view.preedit_sel_end = snapshot.preedit_sel_end;
    view.preedit_cursor_pos = snapshot.preedit_cursor_pos;
    view.status_flags = snapshot.status_flags;
    view.is_last_page = snapshot.is_last_page;

    unsigned char* strings =
        allocation + candidate_offset + snapshot.candidates.size() * stride;
    auto copy_string = [&strings](const std::string& source) {
      FamoUtf8String target{};
      target.size = static_cast<uint32_t>(sizeof(target));
      if (!source.empty()) {
        std::memcpy(strings, source.c_str(), source.size() + 1);
        target.data = reinterpret_cast<const char*>(strings);
        target.length_bytes = static_cast<uint32_t>(source.size());
        strings += source.size() + 1;
      }
      return target;
    };
    view.preedit = copy_string(snapshot.preedit);
    view.commit = copy_string(snapshot.commit);
    view.commit_preview = copy_string(snapshot.commit_preview);
    view.schema_id = copy_string(snapshot.schema_id);
    view.schema_name = copy_string(snapshot.schema_name);

    if (!snapshot.candidates.empty()) {
      view.candidates = reinterpret_cast<const FamoCandidateV2*>(
          allocation + candidate_offset);
      for (size_t i = 0; i < snapshot.candidates.size(); ++i) {
        auto* candidate = reinterpret_cast<FamoCandidateV2*>(
            allocation + candidate_offset + i * stride);
        candidate->struct_size =
            static_cast<uint32_t>(FAMO_CANDIDATE_V2_STRIDE);
        candidate->text = copy_string(snapshot.candidates[i].text);
        candidate->comment = copy_string(snapshot.candidates[i].comment);
        candidate->label = copy_string(snapshot.candidates[i].label);
        candidate->quality = snapshot.candidates[i].quality;
        candidate->flags = snapshot.candidates[i].flags;
      }
    }

    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!accepting_) {
        host.free(allocation);
        return FAMO_ENGINE_E_RUNTIME;
      }
      if (!live_.insert(result).second) {
        host.free(allocation);
        return FAMO_ENGINE_E_RUNTIME;
      }
    } catch (...) {
      host.free(allocation);
      return FAMO_ENGINE_E_RUNTIME;
    }
    *out_result = result;
    return FAMO_ENGINE_OK;
  }

  // Complete and register the only allocation needed by the post-mutation
  // failure path before the engine is allowed to dispatch the business action.
  int32_t PrepareEmergency(const FamoEngineHostApi& host,
                           const FamoEngineActionRequestV2& request,
                           FamoEngineActionResultV2** out_result) {
    Snapshot empty;
    return Publish(host, request, false, empty, out_result,
                   FAMO_ENGINE_RESULT_RESYNC_REQUIRED);
  }

  int32_t Free(const FamoEngineHostApi& host,
               FamoEngineActionResultV2* result) noexcept {
    if (!result || !host.free)
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = live_.find(result);
      if (found == live_.end())
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      live_.erase(found);
    } catch (...) {
      return FAMO_ENGINE_E_RUNTIME;
    }
    host.free(result);
    return FAMO_ENGINE_OK;
  }

  bool Empty() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      return live_.empty();
    } catch (...) {
      return false;
    }
  }

  void Drain(const FamoEngineHostApi& host) noexcept {
    std::unordered_set<FamoEngineActionResultV2*> pending;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_ = false;
      pending.swap(live_);
    } catch (...) {
      return;
    }
    if (host.free) {
      for (auto* result : pending)
        host.free(result);
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_set<FamoEngineActionResultV2*> live_;
  bool accepting_ = false;
};

inline bool ZeroKey(const FamoKeyEvent& key) noexcept {
  return key.size == 0 && key.virtual_key == 0 && key.scan_code == 0 &&
         key.modifiers == 0 && key.is_key_down == 0 &&
         key.timestamp_ms == 0;
}

inline bool IsBusinessAction(uint32_t action) noexcept {
  switch (action) {
    case FAMO_ENGINE_ACTION_PROCESS_KEY:
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE:
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE:
    case FAMO_ENGINE_ACTION_COMMIT_COMPOSITION:
    case FAMO_ENGINE_ACTION_CLEAR_COMPOSITION:
    case FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE:
    case FAMO_ENGINE_ACTION_CHANGE_PAGE:
      return true;
    default:
      return false;
  }
}

inline int32_t ValidateRequest(
    const FamoEngineActionRequestV2* request) noexcept {
  if (!request ||
      request->struct_size < FAMO_ENGINE_ACTION_REQUEST_V2_REQUIRED_SIZE) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  if (request->view_layout_version != FAMO_COMPOSITION_LAYOUT_V2 ||
      request->candidate_layout_version != FAMO_CANDIDATE_LAYOUT_V2 ||
      request->candidate_stride != FAMO_CANDIDATE_V2_STRIDE) {
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  }

  const bool key_is_zero = ZeroKey(request->key);
  switch (request->action) {
    case FAMO_ENGINE_ACTION_PROCESS_KEY: {
      if (request->index != 0 || request->count != 0 || request->value != 0 ||
          request->key.size < FAMO_KEY_EVENT_REQUIRED_SIZE ||
          request->key.is_key_down > 1) {
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      }
      const bool release =
          (request->key.modifiers & FAMO_KEY_V2_MOD_RELEASE) != 0;
      if ((request->key.is_key_down != 0) == release)
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      return FAMO_ENGINE_OK;
    }
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE:
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE:
    case FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE:
      return request->count == 0 && request->value == 0 && key_is_zero
                 ? FAMO_ENGINE_OK
                 : FAMO_ENGINE_E_INVALID_ARGUMENT;
    case FAMO_ENGINE_ACTION_COMMIT_COMPOSITION:
    case FAMO_ENGINE_ACTION_CLEAR_COMPOSITION:
    case FAMO_ENGINE_ACTION_STATUS:
      return request->index == 0 && request->count == 0 &&
                     request->value == 0 && key_is_zero
                 ? FAMO_ENGINE_OK
                 : FAMO_ENGINE_E_INVALID_ARGUMENT;
    case FAMO_ENGINE_ACTION_CHANGE_PAGE:
      return request->index == 0 && request->count == 0 &&
                     (request->value == 0 || request->value == 1) &&
                     key_is_zero
                 ? FAMO_ENGINE_OK
                 : FAMO_ENGINE_E_INVALID_ARGUMENT;
    case FAMO_ENGINE_ACTION_PEEK_CANDIDATES:
      return request->count <= FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES &&
                     request->value == 0 && key_is_zero
                 ? FAMO_ENGINE_OK
                 : FAMO_ENGINE_E_INVALID_ARGUMENT;
    case FAMO_ENGINE_ACTION_RECOVER:
      return request->index == 0 && request->count == 0 && key_is_zero &&
                     request->value > 0 &&
                     IsBusinessAction(static_cast<uint32_t>(request->value))
                 ? FAMO_ENGINE_OK
                 : FAMO_ENGINE_E_INVALID_ARGUMENT;
    default:
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
}

}  // namespace famo_action_v2
