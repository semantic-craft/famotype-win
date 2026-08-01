// FamoTestEngine.dll - a deterministic, dependency-free reference engine.
//
// It implements the full FamoEngineApi v1 table with no librime, no dictionary,
// and no file/network I/O. Given the same keys it always yields the same
// composition, so it is a valid install-smoke / regression replay backend (the
// guide's M4 goal), and it is the conforming engine Tier A uses to prove the
// host loader.
//
// All memory returned in a FamoCompositionView is allocated with the host
// allocator installed at initialize() and released symmetrically in free_view().
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include <windows.h>

#include "../../famo_engine_api.h"
#include "../action_v2_result.h"
#include "../view_abi.h"

namespace {

FamoEngineHostApi g_host;  // captured at initialize()
famo_action_v2::ResultStore g_v2_results;
FamoEngineNotificationHandlerV2 g_notification_handler = nullptr;
void* g_notification_user_data = nullptr;

std::string Environment(const char* name) {
  const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
  if (required <= 1) return {};
  std::string value(static_cast<size_t>(required), '\0');
  const DWORD copied = GetEnvironmentVariableA(name, value.data(), required);
  if (copied != required - 1) return {};
  value.resize(copied);
  return value;
}

// Fixed pinyin-ish table for determinism. Unknown buffers echo themselves so a
// non-empty composition always has at least one candidate.
const std::map<std::string, std::vector<std::string>>& Table() {
  static const std::map<std::string, std::vector<std::string>> t = {
      {"ni", {"\xE4\xBD\xA0", "\xE5\xB0\xBC", "\xE6\xB3\xA5"}},        // 你 尼 泥
      {"hao", {"\xE5\xA5\xBD", "\xE5\x8F\xB7", "\xE8\xB1\xAA"}},      // 好 号 豪
      {"nihao", {"\xE4\xBD\xA0\xE5\xA5\xBD", "\xE4\xBD\xA0\xE5\x8F\xB7"}},  // 你好 你号
      {"famo", {"\xE6\xB3\x95\xE5\xA2\xA8"}},                          // 法墨
  };
  return t;
}

std::vector<std::string> CandidatesFor(const std::string& buffer) {
  if (buffer.empty()) return {};
  const std::string configured_count =
      Environment("FAMO_TEST_CANDIDATE_COUNT");
  if (!configured_count.empty()) {
    const unsigned long count =
        std::strtoul(configured_count.c_str(), nullptr, 10);
    if (count > 0 && count <= FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES) {
      std::vector<std::string> generated;
      generated.reserve(static_cast<size_t>(count));
      for (unsigned long index = 0; index < count; ++index)
        generated.push_back(buffer + std::to_string(index));
      return generated;
    }
  }
  auto it = Table().find(buffer);
  if (it != Table().end()) return it->second;
  return {buffer};  // echo unknown input as a single candidate
}

std::optional<size_t> CandidateSelectionIndex(uint32_t key) {
  if (key > 0x7f)
    return std::nullopt;
  std::string select_keys = Environment("FAMO_TEST_SELECT_KEYS");
  if (select_keys.empty())
    select_keys = "1234567890";
  const size_t index = select_keys.find(static_cast<char>(key));
  return index == std::string::npos ? std::nullopt
                                    : std::optional<size_t>(index);
}

void OverrideCommitSizeForTest(std::string* commit) {
  if (!commit || commit->empty())
    return;
  const std::string configured = Environment("FAMO_TEST_COMMIT_BYTES");
  if (configured.empty())
    return;
  unsigned long long requested =
      std::strtoull(configured.c_str(), nullptr, 10);
  requested = (std::min)(
      requested,
      static_cast<unsigned long long>(
          FAMO_ENGINE_V2_MAX_STRING_BYTES) + 1ull);
  commit->assign(static_cast<size_t>(requested), 'x');
}

// Duplicate a std::string into host-allocated memory as a FamoUtf8String.
FamoUtf8String Dup(const std::string& s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = nullptr;
  v.length_bytes = 0;
  if (!g_host.alloc) return v;
  char* p = static_cast<char*>(g_host.alloc(s.size() + 1));
  if (!p) return v;
  std::memcpy(p, s.c_str(), s.size() + 1);
  v.data = p;
  v.length_bytes = static_cast<uint32_t>(s.size());
  return v;
}

void HostFreeStr(FamoUtf8String* v) {
  if (v && v->data && g_host.free) {
    g_host.free(const_cast<char*>(v->data));
    v->data = nullptr;
    v->length_bytes = 0;
  }
}

void FillCandidates(const std::vector<std::string>& candidates, size_t start,
                    size_t count, uint32_t view_size,
                    FamoCompositionView* out) {
  if (!out || !g_host.alloc || start >= candidates.size() || count == 0)
    return;
  const size_t end = (std::min)(candidates.size(), start + count);
  const size_t size = end - start;
  const size_t stride = famo_view_abi::CandidateStride(view_size);
  void* array = g_host.alloc(stride * size);
  if (!array)
    return;
  for (size_t i = 0; i < size; ++i) {
    FamoCandidate* candidate =
        famo_view_abi::CandidateAt(array, i, stride);
    std::memset(candidate, 0, stride);
    candidate->size = static_cast<uint32_t>(stride);
    candidate->text = Dup(candidates[start + i]);
    candidate->comment = Dup("");
    candidate->flags = i == 0 ? FAMO_CANDIDATE_FLAG_DEFAULT : 0u;
    if (famo_view_abi::HasV12Candidates(view_size)) {
      const char digit[2] = {
          static_cast<char>('0' + ((start + i + 1) % 10)), '\0'};
      candidate->label = Dup(digit);
    }
  }
  out->candidates = static_cast<const FamoCandidate*>(array);
  out->candidate_count = static_cast<uint32_t>(size);
}

// A static (non-owned) FamoUtf8String pointing at a literal, for get_info.
FamoUtf8String Static(const char* s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = static_cast<uint32_t>(std::strlen(s));
  return v;
}

void FillView(const std::string& buffer, const std::string& commit, bool handled,
              uint32_t view_size, FamoCompositionView* out) {
  FamoCompositionView result;
  famo_view_abi::BeginResult(&result, view_size);
  std::string preedit = buffer;
  uint32_t selection_start = 0;
  uint32_t selection_end = static_cast<uint32_t>(buffer.size());
  uint32_t cursor = selection_end;
  if (!Environment("FAMO_TEST_PREEDIT_OFFSETS").empty() && !buffer.empty()) {
    switch (buffer.size()) {
      case 1:
        preedit = "abcd";
        selection_end = 0;
        cursor = 2;
        break;
      case 2:
        preedit = "abcdef";
        selection_start = 1;
        selection_end = 4;
        cursor = 4;
        break;
      case 3:
        preedit = "\xe4\xbd\xa0" "A" "\xe5\xa5\xbd";
        selection_end = 0;
        cursor = 4;
        break;
      default:
        preedit = "\xf0\x9f\x98\x80" "A";
        selection_end = 0;
        cursor = 4;
        break;
    }
  }
  result.preedit = Dup(preedit);
  result.commit = Dup(commit);

  std::vector<std::string> cands = CandidatesFor(buffer);
  FillCandidates(cands, 0, cands.size(), view_size, &result);
  const bool multipage = !Environment("FAMO_TEST_MULTIPAGE").empty() &&
                         result.candidate_count > 1;
  result.page_size = multipage ? 1u : result.candidate_count;

  uint32_t flags = 0;
  if (!preedit.empty()) flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (!commit.empty()) flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (result.candidate_count) flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  if (handled) flags |= FAMO_COMPOSITION_HANDLED;
  result.state_flags = flags;

  // v1.1 deterministic fields (exercise size negotiation + free_view teardown).
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, preedit_sel_start),
          sizeof(result.preedit_sel_start))) {
    result.preedit_sel_start = selection_start;
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, preedit_sel_end),
          sizeof(result.preedit_sel_end))) {
    result.preedit_sel_end = selection_end;
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, preedit_cursor_pos),
          sizeof(result.preedit_cursor_pos))) {
    result.preedit_cursor_pos = cursor;
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, commit_preview),
          sizeof(result.commit_preview))) {
    result.commit_preview =
        Dup(buffer.empty() ? std::string() : CandidatesFor(buffer).front());
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_id),
          sizeof(result.schema_id))) {
    result.schema_id = Dup("test");
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_name),
          sizeof(result.schema_name))) {
    result.schema_name = Dup("Test Engine");
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, status_flags),
          sizeof(result.status_flags))) {
    result.status_flags =
        (Environment("FAMO_TEST_SIMPLIFIED").empty()
             ? 0u
             : static_cast<uint32_t>(FAMO_STATUS_SIMPLIFIED)) |
        (FAMO_STATUS_COMPOSING * (buffer.empty() ? 0u : 1u));
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, is_last_page),
          sizeof(result.is_last_page))) {
    result.is_last_page = multipage ? 0u : 1u;
  }
  famo_view_abi::Publish(out, result, view_size);
}

famo_action_v2::Snapshot SnapshotFor(const std::string& buffer,
                                     const std::string& commit,
                                     uint32_t option_status_flags = 0,
                                     uint32_t highlighted_index = 0) {
  famo_action_v2::Snapshot snapshot;
  snapshot.preedit = buffer;
  snapshot.preedit_sel_end = static_cast<uint32_t>(snapshot.preedit.size());
  snapshot.preedit_cursor_pos = snapshot.preedit_sel_end;
  if (!Environment("FAMO_TEST_PREEDIT_OFFSETS").empty() && !buffer.empty()) {
    switch (buffer.size()) {
      case 1:
        snapshot.preedit = "abcd";
        snapshot.preedit_sel_end = 0;
        snapshot.preedit_cursor_pos = 2;
        break;
      case 2:
        snapshot.preedit = "abcdef";
        snapshot.preedit_sel_start = 1;
        snapshot.preedit_sel_end = 4;
        snapshot.preedit_cursor_pos = 4;
        break;
      case 3:
        snapshot.preedit = "\xe4\xbd\xa0" "A" "\xe5\xa5\xbd";
        snapshot.preedit_sel_end = 0;
        snapshot.preedit_cursor_pos = 4;
        break;
      default:
        snapshot.preedit = "\xf0\x9f\x98\x80" "A";
        snapshot.preedit_sel_end = 0;
        snapshot.preedit_cursor_pos = 4;
        break;
    }
  }
  snapshot.commit = commit;
  const auto candidates = CandidatesFor(buffer);
  snapshot.candidates.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    famo_action_v2::CandidateSnapshot candidate;
    candidate.text = candidates[i];
    candidate.label =
        std::string(1, static_cast<char>('0' + ((i + 1) % 10)));
    candidate.flags = i == 0 ? FAMO_CANDIDATE_FLAG_DEFAULT : 0u;
    snapshot.candidates.push_back(std::move(candidate));
  }
  snapshot.highlighted_index =
      snapshot.candidates.empty()
          ? 0u
          : (std::min)(highlighted_index,
                       static_cast<uint32_t>(snapshot.candidates.size() - 1));
  const bool multipage = !Environment("FAMO_TEST_MULTIPAGE").empty() &&
                         snapshot.candidates.size() > 1;
  snapshot.page_size =
      multipage ? 1u : static_cast<uint32_t>(snapshot.candidates.size());
  if (!snapshot.preedit.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (!snapshot.commit.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (!snapshot.candidates.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  snapshot.commit_preview =
      buffer.empty() || candidates.empty() ? std::string()
                                           : candidates[snapshot.highlighted_index];
  snapshot.schema_id = "test";
  snapshot.schema_name = "Test Engine";
  snapshot.status_flags =
      (Environment("FAMO_TEST_SIMPLIFIED").empty()
           ? 0u
           : static_cast<uint32_t>(FAMO_STATUS_SIMPLIFIED)) |
      (buffer.empty() ? 0u : FAMO_STATUS_COMPOSING) |
      option_status_flags;
  snapshot.is_last_page = multipage ? 0u : 1u;
  return snapshot;
}

}  // namespace

// Opaque context is a real struct inside the DLL; only UTF-8 crosses the ABI.
struct FamoEngineContext {
  std::string buffer;
  std::string pending_commit;
  uint32_t highlighted_index = 0;
  std::map<std::string, std::string> properties;
  std::map<std::string, int32_t> options;
  bool recovery_required = false;
  uint32_t recovery_action = 0;
  bool recovery_handled = false;
  bool has_pending_snapshot = false;
  famo_action_v2::Snapshot pending_snapshot;
  std::string recovery_commit;
  uint32_t process_count = 0;
  uint32_t select_count = 0;
  uint32_t commit_count = 0;
};

alignas(FamoEngineContext) unsigned char
    g_reused_context_storage[sizeof(FamoEngineContext)];
bool g_reused_context_live = false;
FamoEngineContext* g_failed_destroy_context = nullptr;

namespace {

uint32_t OptionStatusFlags(const FamoEngineContext* context) {
  if (!context)
    return 0;
  uint32_t flags = 0;
  const auto ascii = context->options.find("ascii_mode");
  if (ascii != context->options.end() && ascii->second != 0)
    flags |= FAMO_STATUS_ASCII_MODE;
  const auto punct = context->options.find("ascii_punct");
  if (punct != context->options.end() && punct->second != 0)
    flags |= FAMO_STATUS_ASCII_PUNCT;
  const auto full_shape = context->options.find("full_shape");
  if (full_shape != context->options.end() && full_shape->second != 0)
    flags |= FAMO_STATUS_FULL_SHAPE;
  return flags;
}

int32_t FAMO_ENGINE_CALL TeGetInfo(FamoEngineInfo* out_info) {
  if (!out_info || out_info->size < FAMO_ENGINE_INFO_REQUIRED_SIZE)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  out_info->size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  out_info->abi_version = FAMO_ENGINE_ABI_VERSION;
  out_info->capabilities = 0;  // deterministic stub: no Lua/OpenCC/userdb/deploy
  out_info->engine_name = Static("FamoTestEngine");
  out_info->engine_version = Static("1.0.0");
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeGetInfoV2(FamoEngineInfo* out_info) {
  const int32_t result = TeGetInfo(out_info);
  if (result == FAMO_ENGINE_OK)
    out_info->abi_version = FAMO_ENGINE_ABI_V2;
  return result;
}

int32_t FAMO_ENGINE_CALL TeInitialize(const FamoEngineHostApi* host,
                                      const FamoUtf8String* /*data_root*/) {
  if (!host ||
      host->size < static_cast<uint32_t>(FAMO_ENGINE_HOST_API_REQUIRED_SIZE) ||
      host->abi_version != FAMO_ENGINE_ABI_VERSION || !host->alloc ||
      !host->free) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  g_host = *host;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeInitializeV2(
    const FamoEngineHostApi* host,
    const FamoUtf8String* /*data_root*/) {
  if (!host ||
      host->size < static_cast<uint32_t>(FAMO_ENGINE_HOST_API_REQUIRED_SIZE) ||
      host->abi_version != FAMO_ENGINE_ABI_V2 || !host->alloc ||
      !host->free) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  g_host = *host;
  if (!g_v2_results.Start()) {
    std::memset(&g_host, 0, sizeof(g_host));
    return FAMO_ENGINE_E_RUNTIME;
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeShutdown(void) {
  g_v2_results.Drain(g_host);
  if (g_reused_context_live) {
    reinterpret_cast<FamoEngineContext*>(g_reused_context_storage)
        ->~FamoEngineContext();
    g_reused_context_live = false;
  }
  g_failed_destroy_context = nullptr;
  g_notification_handler = nullptr;
  g_notification_user_data = nullptr;
  std::memset(&g_host, 0, sizeof(g_host));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeCreateContext(const FamoUtf8String* schema_id,
                                         FamoEngineContext** out_context) {
  if (!out_context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_context = nullptr;
  const std::string rejected = Environment("FAMO_TEST_FAIL_SCHEMA");
  const std::string_view schema(
      schema_id && schema_id->data ? schema_id->data : "",
      schema_id && schema_id->data ? schema_id->length_bytes : 0);
  if (!rejected.empty() && schema == rejected)
    return FAMO_ENGINE_E_SCHEMA;
  if (schema == "notify_then_fail") {
    // Model a synchronous create-time notification, but obey the ABI rule:
    // it is staged and discarded because the operation will fail. The next
    // special create deliberately reuses this exact address.
    auto* staged = new (g_reused_context_storage) FamoEngineContext();
    staged->buffer = "discarded-create-notification";
    staged->~FamoEngineContext();
    return FAMO_ENGINE_E_RUNTIME;
  }
  if (schema == "reuse_after_failed_create") {
    if (g_reused_context_live)
      return FAMO_ENGINE_E_RUNTIME;
    auto* reused = new (g_reused_context_storage) FamoEngineContext();
    g_reused_context_live = true;
    *out_context = reused;
    if (g_notification_handler) {
      const FamoUtf8String type = Static("option");
      const FamoUtf8String value = Static("create_reuse_fresh");
      const FamoUtf8String label = Static("Fresh reused context");
      g_notification_handler(g_notification_user_data, reused, &type, &value,
                             &label);
    }
    return FAMO_ENGINE_OK;
  }
  *out_context = new (std::nothrow) FamoEngineContext();
  return *out_context ? FAMO_ENGINE_OK : FAMO_ENGINE_E_RUNTIME;
}

int32_t FAMO_ENGINE_CALL TeDestroyContext(FamoEngineContext* context) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (!Environment("FAMO_TEST_FAIL_DESTROY").empty()) {
    if (!g_failed_destroy_context)
      g_failed_destroy_context = context;
    return FAMO_ENGINE_E_RUNTIME;
  }
  if (g_failed_destroy_context == context) {
    SetEnvironmentVariableA("FAMO_TEST_DESTROY_RETRY_OBSERVED", "1");
    g_failed_destroy_context = nullptr;
  }
  if (context ==
      reinterpret_cast<FamoEngineContext*>(g_reused_context_storage)) {
    if (!g_reused_context_live)
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
    context->~FamoEngineContext();
    g_reused_context_live = false;
    return FAMO_ENGINE_OK;
  }
  delete context;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeProcessKey(FamoEngineContext* context,
                                      const FamoKeyEvent* key,
                                      FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !key ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }

  // A Shift release is handled only when the host supplies librime's expanded
  // release bit.  The TSF integration check uses this to guard the direct
  // host-to-engine mapping (Weasel's IPC wire format uses bit 14 instead).
  if (key->is_key_down != 1) {
    constexpr uint32_t kRimeShiftLeft = 0xffe1;
    constexpr uint32_t kRimeShiftRight = 0xffe2;
    constexpr uint32_t kRimeReleaseMask = 1u << 30;
    const bool handled =
        (key->virtual_key == kRimeShiftLeft ||
         key->virtual_key == kRimeShiftRight) &&
        (key->modifiers & kRimeReleaseMask) != 0;
    FillView(context->buffer, "", handled, view_size, out_view);
    return FAMO_ENGINE_OK;
  }

  const uint32_t vk = key->virtual_key;
  if (vk == 0xff60 &&
      !Environment("FAMO_TEST_UNHANDLED_SELECTION").empty()) {
    FillView(context->buffer, "unexpected-select", true, view_size, out_view);
    return FAMO_ENGINE_OK;
  }
  if (vk == 0xff60 && !context->pending_commit.empty()) {
    std::string commit = std::move(context->pending_commit);
    FillView(context->buffer, commit, false, view_size, out_view);
    return FAMO_ENGINE_OK;
  }

  bool handled = false;
  const auto selected = CandidateSelectionIndex(vk);
  if (!context->buffer.empty() && selected) {
    const std::vector<std::string> candidates =
        CandidatesFor(context->buffer);
    if (*selected < candidates.size()) {
      const std::string commit = candidates[*selected];
      context->buffer.clear();
      FillView(context->buffer, commit, true, view_size, out_view);
      return FAMO_ENGINE_OK;
    }
  } else if (context->buffer.empty() && vk >= '0' && vk <= '9' &&
             !Environment("FAMO_TEST_DIGIT_INPUT").empty()) {
    // Some schemas legitimately start composition with a digit. Keep the
    // normal non-empty select-key interpretation above, but let TSF integration
    // prove that an empty preedit does not pre-consume the physical key.
    context->buffer.push_back(static_cast<char>(vk));
    handled = true;
  } else if (vk >= 'a' && vk <= 'z') {
    context->buffer.push_back(static_cast<char>(vk));
    handled = true;
  } else if (vk >= 'A' && vk <= 'Z') {
    context->buffer.push_back(static_cast<char>(vk + 32));  // normalize to lower
    handled = true;
  } else if (vk == 8) {  // Backspace
    if (!context->buffer.empty()) {
      context->buffer.pop_back();
      handled = true;
    }
  } else if (vk == 27) {  // Esc
    if (!context->buffer.empty()) {
      context->buffer.clear();
      handled = true;
    }
  } else if (vk == ' ' || vk == 13) {  // Space / Enter -> commit highlighted
    if (context->buffer.empty()) {
      FillView(context->buffer, "", false, view_size, out_view);
      return FAMO_ENGINE_OK;
    }
    std::vector<std::string> cands = CandidatesFor(context->buffer);
    std::string commit = cands.empty() ? context->buffer : cands[0];
    context->buffer.clear();
    FillView(context->buffer, commit, true, view_size, out_view);
    return FAMO_ENGINE_OK;
  }

  FillView(context->buffer, "", handled, view_size, out_view);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSelectCandidate(FamoEngineContext* context, uint32_t index,
                                           FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !famo_view_abi::Negotiate(out_view, &view_size))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::vector<std::string> cands = CandidatesFor(context->buffer);
  if (index >= cands.size()) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (!Environment("FAMO_TEST_UNHANDLED_SELECTION").empty()) {
    FillView(context->buffer, "", false, view_size, out_view);
    return FAMO_ENGINE_OK;
  }
  std::string commit = cands[index];
  context->buffer.clear();
  if (!Environment("FAMO_TEST_DEFER_SELECTION_COMMIT").empty()) {
    context->pending_commit = std::move(commit);
    FillView(context->buffer, "", true, view_size, out_view);
    return FAMO_ENGINE_OK;
  }
  FillView(context->buffer, commit, true, view_size, out_view);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name,
                                     int32_t value) {
  if (!context || !name) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (context->recovery_required)
    return FAMO_ENGINE_E_RECOVERY_REQUIRED;
  const std::string fail = Environment("FAMO_TEST_FAIL_OPTION");
  const std::string_view option(name->data ? name->data : "", name->length_bytes);
  if (!fail.empty() && option == fail) return FAMO_ENGINE_E_RUNTIME;
  const std::string fail_rollback =
      Environment("FAMO_TEST_FAIL_OPTION_ROLLBACK");
  if (value == 0 && !fail_rollback.empty() && option == fail_rollback)
    return FAMO_ENGINE_E_RUNTIME;
  const std::string fail_buffer =
      Environment("FAMO_TEST_FAIL_OPTION_BUFFER");
  if (!fail_buffer.empty() && context->buffer == fail_buffer)
    return FAMO_ENGINE_E_RUNTIME;
  if (option == "notify_test" && g_notification_handler) {
    const FamoUtf8String type = Static("option");
    const FamoUtf8String message =
        Static(value ? "notify_test" : "!notify_test");
    const FamoUtf8String label =
        Static(value ? "Test enabled" : "Test disabled");
    g_notification_handler(g_notification_user_data, context, &type, &message,
                           &label);
  } else if (option == "notify_client_app" && g_notification_handler) {
    const auto found = context->properties.find("client_app");
    const std::string label =
        found == context->properties.end() ? std::string() : found->second;
    const FamoUtf8String type = Static("option");
    const FamoUtf8String message = Static("notify_client_app");
    const FamoUtf8String label_view{
        static_cast<uint32_t>(sizeof(FamoUtf8String)),
        label.empty() ? nullptr : label.data(),
        static_cast<uint32_t>(label.size())};
    g_notification_handler(g_notification_user_data, context, &type, &message,
                           &label_view);
  } else if (option == "notify_invalid" && g_notification_handler) {
    const char invalid[] = {static_cast<char>(0xc0),
                            static_cast<char>(0xaf)};
    const FamoUtf8String type = Static("option");
    const FamoUtf8String message{
        static_cast<uint32_t>(sizeof(FamoUtf8String)), invalid, 2};
    const FamoUtf8String label = Static("");
    g_notification_handler(g_notification_user_data, context, &type, &message,
                           &label);
  } else if (option == "notify_null_context" && g_notification_handler) {
    const FamoUtf8String type = Static("option");
    const FamoUtf8String message = Static("notify_null_context");
    const FamoUtf8String label = Static("should be rejected");
    g_notification_handler(g_notification_user_data, nullptr, &type, &message,
                           &label);
  }
  context->options[std::string(option)] = value;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSetPropertyV2(
    FamoEngineContext* context,
    const FamoUtf8String* name,
    const FamoUtf8String* value) {
  if (!context || !name || !value)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (context->recovery_required)
    return FAMO_ENGINE_E_RECOVERY_REQUIRED;
  const std::string property_name =
      name->data ? std::string(name->data, name->length_bytes) : std::string();
  const std::string property_value =
      value->data ? std::string(value->data, value->length_bytes)
                  : std::string();
  context->properties[property_name] = property_value;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSetNotificationHandlerV2(
    FamoEngineNotificationHandlerV2 handler,
    void* user_data) {
  if (!handler && user_data)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_notification_handler = handler;
  g_notification_user_data = handler ? user_data : nullptr;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeDeploySchema(const FamoUtf8String* /*schema_id*/,
                                        FamoUtf8String* /*out_error_message*/) {
  const std::string delay = Environment("FAMO_TEST_DEPLOY_DELAY_MS");
  if (!delay.empty()) {
    const int milliseconds = std::atoi(delay.c_str());
    if (milliseconds > 0 && milliseconds <= 5000)
      std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  }
  if (!Environment("FAMO_TEST_DEPLOY_FAIL").empty())
    return FAMO_ENGINE_E_RUNTIME;
  return FAMO_ENGINE_OK;  // stub: no dictionaries to deploy
}

int32_t FAMO_ENGINE_CALL TeFreeView(FamoCompositionView* view) {
  uint32_t view_size = 0;
  if (!famo_view_abi::Negotiate(view, &view_size))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  HostFreeStr(&view->preedit);
  HostFreeStr(&view->commit);
  if (view->candidates && g_host.free) {
    void* candidates =
        const_cast<FamoCandidate*>(view->candidates);
    const size_t stride = famo_view_abi::CandidateStride(view_size);
    for (uint32_t i = 0; i < view->candidate_count; ++i) {
      FamoCandidate* candidate =
          famo_view_abi::CandidateAt(candidates, i, stride);
      HostFreeStr(&candidate->text);
      HostFreeStr(&candidate->comment);
      if (famo_view_abi::HasV12Candidates(view_size))
        HostFreeStr(&candidate->label);
    }
    g_host.free(candidates);
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, commit_preview),
          sizeof(view->commit_preview))) {
    HostFreeStr(&view->commit_preview);
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_id),
          sizeof(view->schema_id))) {
    HostFreeStr(&view->schema_id);
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_name),
          sizeof(view->schema_name))) {
    HostFreeStr(&view->schema_name);
  }
  famo_view_abi::ClearPreservingSize(view, view_size);
  return FAMO_ENGINE_OK;
}

// v1.1 deterministic stubs.
int32_t FAMO_ENGINE_CALL TeGetStatus(FamoEngineContext* context,
                                     FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !famo_view_abi::Negotiate(out_view, &view_size))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  FillView(context->buffer, "", false, view_size,
           out_view);  // status folded in; no commit
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeGetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name, int32_t* out_value) {
  if (!context || !name || !out_value) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string_view option(
      name->data ? name->data : "", name->length_bytes);
  if (option == "test_process_count")
    *out_value = static_cast<int32_t>(context->process_count);
  else if (option == "test_select_count")
    *out_value = static_cast<int32_t>(context->select_count);
  else if (option == "test_commit_count")
    *out_value = static_cast<int32_t>(context->commit_count);
  else {
    const auto found = context->options.find(std::string(option));
    *out_value = found == context->options.end() ? 0 : found->second;
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeCommitComposition(FamoEngineContext* context) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  context->buffer.clear();
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeClearComposition(FamoEngineContext* context) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  context->buffer.clear();
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeHighlightCandidate(FamoEngineContext* context, uint32_t /*index*/) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return FAMO_ENGINE_OK;  // stub: no paging state
}

int32_t FAMO_ENGINE_CALL TeChangePage(FamoEngineContext* context, int32_t /*backward*/) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return FAMO_ENGINE_OK;  // stub: single page
}

int32_t FAMO_ENGINE_CALL TePeekCandidates(FamoEngineContext* context,
                                          uint32_t index, uint32_t count,
                                          FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || count > 64 ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  FamoCompositionView result;
  famo_view_abi::BeginResult(&result, view_size);
  FillCandidates(CandidatesFor(context->buffer), index, count, view_size,
                 &result);
  famo_view_abi::Publish(out_view, result, view_size);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSelectCandidateAbsolute(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) {
  return TeSelectCandidate(context, index, out_view);
}

bool FailPostMutationSnapshot(uint32_t action) {
  const std::string fail =
      Environment("FAMO_TEST_FAIL_POST_MUTATION_SNAPSHOT");
  if (fail.empty())
    return false;
  if (fail == "*" || fail == "1")
    return true;
  return (action == FAMO_ENGINE_ACTION_PROCESS_KEY && fail == "process") ||
         ((action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE ||
           action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE) &&
          fail == "select") ||
         (action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION &&
          fail == "commit");
}

void MarkTestRecovery(FamoEngineContext* context,
                      const FamoEngineActionRequestV2& request,
                      bool handled,
                      famo_action_v2::Snapshot* completed_snapshot) noexcept {
  context->recovery_action = request.action;
  context->recovery_handled = handled;
  context->recovery_required = true;
  if (completed_snapshot) {
    context->pending_snapshot = std::move(*completed_snapshot);
    context->has_pending_snapshot = true;
    context->recovery_commit.clear();
  }
}

void ClearTestRecovery(FamoEngineContext* context) noexcept {
  context->recovery_required = false;
  context->recovery_action = 0;
  context->recovery_handled = false;
  context->has_pending_snapshot = false;
  context->pending_snapshot = famo_action_v2::Snapshot{};
  context->recovery_commit.clear();
}

void CorruptEmergencyReceiptForTest(
    FamoEngineActionResultV2* receipt) noexcept {
  if (receipt &&
      !Environment("FAMO_TEST_MALFORMED_RESYNC_RECEIPT").empty()) {
    // The host must authenticate only the irreversible receipt envelope and
    // ignore this untrusted UI payload before issuing RECOVER.
    receipt->view.struct_size = 0;
    receipt->view.preedit_sel_end = 1;
  }
}

int32_t RecoverTestAction(
    FamoEngineContext* context,
    const FamoEngineActionRequestV2& recovery_request,
    FamoEngineActionResultV2** out_result) {
  const uint32_t original_action =
      static_cast<uint32_t>(recovery_request.value);
  if (!context->recovery_required ||
      original_action != context->recovery_action) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  if (!context->has_pending_snapshot) {
    famo_action_v2::Snapshot snapshot =
        SnapshotFor(context->buffer, context->recovery_commit,
                    OptionStatusFlags(context), context->highlighted_index);
    if (!famo_action_v2::TrimOptionalToResultBudget(&snapshot))
      return FAMO_ENGINE_E_RUNTIME;
    context->pending_snapshot = std::move(snapshot);
    context->has_pending_snapshot = true;
    context->recovery_commit.clear();
  }
  FamoEngineActionRequestV2 original_request = recovery_request;
  original_request.action = original_action;
  original_request.value = 0;
  const int32_t publish_result = g_v2_results.Publish(
      g_host, original_request, context->recovery_handled,
      context->pending_snapshot, out_result);
  if (publish_result != FAMO_ENGINE_OK)
    return publish_result;
  ClearTestRecovery(context);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeExecuteActionV2(
    FamoEngineContext* context, const FamoEngineActionRequestV2* request,
    FamoEngineActionResultV2** out_result) {
  if (!out_result)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_result = nullptr;
  if (!context || !request ||
      famo_action_v2::ValidateRequest(request) != FAMO_ENGINE_OK) {
    return context ? famo_action_v2::ValidateRequest(request)
                   : FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  try {
    if (request->action == FAMO_ENGINE_ACTION_RECOVER)
      return RecoverTestAction(context, *request, out_result);
    if (context->recovery_required &&
        famo_action_v2::IsBusinessAction(request->action)) {
      return FAMO_ENGINE_E_RECOVERY_REQUIRED;
    }
    if (request->action == FAMO_ENGINE_ACTION_STATUS) {
      famo_action_v2::Snapshot snapshot =
          SnapshotFor(context->buffer, "", OptionStatusFlags(context),
                      context->highlighted_index);
      if (!famo_action_v2::TrimOptionalToResultBudget(&snapshot))
        return FAMO_ENGINE_E_RUNTIME;
      return g_v2_results.Publish(g_host, *request, false, snapshot,
                                  out_result);
    }
    if (request->action == FAMO_ENGINE_ACTION_PEEK_CANDIDATES) {
      if (request->count > FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES)
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      famo_action_v2::Snapshot snapshot;
      const auto candidates = CandidatesFor(context->buffer);
      const size_t start = request->index;
      const size_t end =
          start >= candidates.size()
              ? start
              : (std::min)(candidates.size(),
                           start + static_cast<size_t>(request->count));
      for (size_t index = start; index < end; ++index) {
        famo_action_v2::CandidateSnapshot candidate;
        candidate.text = candidates[index];
        candidate.label = std::string(
            1, static_cast<char>('0' + ((index + 1) % 10)));
        snapshot.candidates.push_back(std::move(candidate));
      }
      if (!Environment("FAMO_TEST_PEEK_OVERRUN").empty()) {
        famo_action_v2::CandidateSnapshot overflow;
        overflow.text = "overflow";
        overflow.label = "!";
        snapshot.candidates.push_back(std::move(overflow));
      }
      if (!snapshot.candidates.empty()) {
        snapshot.state_flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
        snapshot.page_size =
            static_cast<uint32_t>(snapshot.candidates.size());
      }
      if (!famo_action_v2::TrimOptionalToResultBudget(&snapshot))
        return FAMO_ENGINE_E_RUNTIME;
      return g_v2_results.Publish(g_host, *request, false, snapshot,
                                  out_result);
    }

    // Build every potentially allocating mutation input before the emergency
    // receipt. After PrepareEmergency succeeds, the test engine changes only
    // scalars and moves already-owned strings until a final snapshot exists.
    std::string next_buffer = context->buffer;
    uint32_t next_highlighted_index = context->highlighted_index;
    bool handled = false;
    std::string commit;
    switch (request->action) {
      case FAMO_ENGINE_ACTION_PROCESS_KEY:
        if (request->key.is_key_down != 1) {
          constexpr uint32_t kRimeShiftLeft = 0xffe1;
          constexpr uint32_t kRimeShiftRight = 0xffe2;
          constexpr uint32_t kRimeReleaseMask = 1u << 30;
          handled =
              (request->key.virtual_key == kRimeShiftLeft ||
               request->key.virtual_key == kRimeShiftRight) &&
              (request->key.modifiers & kRimeReleaseMask) != 0;
        } else {
          const uint32_t key = request->key.virtual_key;
          const auto selected = CandidateSelectionIndex(key);
          if (!next_buffer.empty() && selected) {
            const auto candidates = CandidatesFor(next_buffer);
            if (*selected < candidates.size()) {
              commit = candidates[*selected];
              next_buffer.clear();
              next_highlighted_index = 0;
              handled = true;
            }
          } else if (next_buffer.empty() && key >= '0' && key <= '9' &&
                     !Environment("FAMO_TEST_DIGIT_INPUT").empty()) {
            next_buffer.push_back(static_cast<char>(key));
            next_highlighted_index = 0;
            handled = true;
          } else if (key >= 'a' && key <= 'z') {
            next_buffer.push_back(static_cast<char>(key));
            next_highlighted_index = 0;
            handled = true;
          } else if (key >= 'A' && key <= 'Z') {
            next_buffer.push_back(static_cast<char>(key + 32));
            next_highlighted_index = 0;
            handled = true;
          } else if (key == 8 && !next_buffer.empty()) {
            next_buffer.pop_back();
            next_highlighted_index = 0;
            handled = true;
          } else if (key == 27 && !next_buffer.empty()) {
            next_buffer.clear();
            next_highlighted_index = 0;
            handled = true;
          } else if ((key == ' ' || key == 13) &&
                     !next_buffer.empty()) {
            const auto candidates = CandidatesFor(next_buffer);
            commit = candidates.empty()
                         ? next_buffer
                         : candidates[(std::min)(
                               next_highlighted_index,
                               static_cast<uint32_t>(candidates.size() - 1))];
            next_buffer.clear();
            next_highlighted_index = 0;
            handled = true;
          }
        }
        break;
      case FAMO_ENGINE_ACTION_SELECT_CANDIDATE:
      case FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE: {
        const auto candidates = CandidatesFor(next_buffer);
        if (request->index >= candidates.size())
          return FAMO_ENGINE_E_INVALID_ARGUMENT;
        if (Environment("FAMO_TEST_UNHANDLED_SELECTION").empty()) {
          commit = candidates[request->index];
          next_buffer.clear();
          next_highlighted_index = 0;
          handled = true;
        }
        break;
      }
      case FAMO_ENGINE_ACTION_COMMIT_COMPOSITION:
        if (!next_buffer.empty()) {
          const auto candidates = CandidatesFor(next_buffer);
          commit = candidates.empty()
                       ? next_buffer
                       : candidates[(std::min)(
                             next_highlighted_index,
                             static_cast<uint32_t>(candidates.size() - 1))];
          if (!Environment("FAMO_TEST_FORMAT_COMMIT").empty())
            commit = "\xE3\x80\x8C" + commit + "\xE3\x80\x8D";  // 「...」
          next_buffer.clear();
          next_highlighted_index = 0;
          handled = true;
        }
        break;
      case FAMO_ENGINE_ACTION_CLEAR_COMPOSITION:
        next_buffer.clear();
        next_highlighted_index = 0;
        if (!Environment("FAMO_TEST_NONEMPTY_CLEAR_REPLY").empty())
          commit = "unexpected-clear";
        handled = true;
        break;
      case FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE: {
        const auto candidates = CandidatesFor(next_buffer);
        if (request->index >= candidates.size())
          return FAMO_ENGINE_E_INVALID_ARGUMENT;
        next_highlighted_index = request->index;
        handled = true;
        break;
      }
      case FAMO_ENGINE_ACTION_CHANGE_PAGE:
        handled = true;
        break;
      default:
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
    }
    OverrideCommitSizeForTest(&commit);
    if (commit.size() > FAMO_ENGINE_V2_MAX_STRING_BYTES)
      return FAMO_ENGINE_E_RUNTIME;

    const bool fail_snapshot =
        FailPostMutationSnapshot(request->action);
    FamoEngineActionResultV2* emergency = nullptr;
    const int32_t emergency_result =
        g_v2_results.PrepareEmergency(g_host, *request, &emergency);
    if (emergency_result != FAMO_ENGINE_OK)
      return emergency_result;

    if (request->action == FAMO_ENGINE_ACTION_PROCESS_KEY)
      ++context->process_count;
    else if (request->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE ||
             request->action ==
                 FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE)
      ++context->select_count;
    else if (request->action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION)
      ++context->commit_count;
    context->buffer = std::move(next_buffer);
    context->highlighted_index = next_highlighted_index;
    context->recovery_commit = std::move(commit);

    famo_action_v2::Snapshot snapshot;
    bool snapshot_complete = false;
    if (!fail_snapshot) {
      try {
        snapshot = SnapshotFor(context->buffer, context->recovery_commit,
                               OptionStatusFlags(context),
                               context->highlighted_index);
        snapshot_complete =
            famo_action_v2::TrimOptionalToResultBudget(&snapshot);
      } catch (...) {
        snapshot_complete = false;
      }
    }
    if (!snapshot_complete) {
      MarkTestRecovery(context, *request, handled, nullptr);
      emergency->handled = handled ? 1u : 0u;
      CorruptEmergencyReceiptForTest(emergency);
      *out_result = emergency;
      return FAMO_ENGINE_OK;
    }

    FamoEngineActionResultV2* completed = nullptr;
    const int32_t publish_result =
        g_v2_results.Publish(g_host, *request, handled, snapshot, &completed);
    if (publish_result != FAMO_ENGINE_OK) {
      MarkTestRecovery(context, *request, handled, &snapshot);
      emergency->handled = handled ? 1u : 0u;
      CorruptEmergencyReceiptForTest(emergency);
      *out_result = emergency;
      return FAMO_ENGINE_OK;
    }

    ClearTestRecovery(context);
    (void)g_v2_results.Free(g_host, emergency);
    *out_result = completed;
    return FAMO_ENGINE_OK;
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeFreeResultV2(
    FamoEngineActionResultV2* result) {
  return g_v2_results.Free(g_host, result);
}

template <typename Callback>
int32_t TeLegacyNoUnwind(Callback callback) noexcept {
  try {
    return callback();
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeGetInfoSafe(
    FamoEngineInfo* out_info) noexcept {
  return TeLegacyNoUnwind([&] { return TeGetInfo(out_info); });
}

int32_t FAMO_ENGINE_CALL TeInitializeSafe(
    const FamoEngineHostApi* host,
    const FamoUtf8String* data_root) noexcept {
  if (!famo_action_v2::ValidInputString(data_root))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const int32_t result =
      TeLegacyNoUnwind([&] { return TeInitialize(host, data_root); });
  if (result != FAMO_ENGINE_OK)
    std::memset(&g_host, 0, sizeof(g_host));
  return result;
}

int32_t FAMO_ENGINE_CALL TeShutdownSafe() noexcept {
  const int32_t result = TeLegacyNoUnwind([] { return TeShutdown(); });
  if (result != FAMO_ENGINE_OK)
    std::memset(&g_host, 0, sizeof(g_host));
  return result;
}

int32_t FAMO_ENGINE_CALL TeCreateContextSafe(
    const FamoUtf8String* schema_id,
    FamoEngineContext** out_context) noexcept {
  if (out_context)
    *out_context = nullptr;
  if (!famo_action_v2::ValidInputString(schema_id, true))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return TeLegacyNoUnwind(
      [&] { return TeCreateContext(schema_id, out_context); });
}

int32_t FAMO_ENGINE_CALL TeDestroyContextSafe(
    FamoEngineContext* context) noexcept {
  return TeLegacyNoUnwind([&] { return TeDestroyContext(context); });
}

int32_t FAMO_ENGINE_CALL TeProcessKeySafe(
    FamoEngineContext* context, const FamoKeyEvent* key,
    FamoCompositionView* out_view) noexcept {
  if (!key || key->size < FAMO_KEY_EVENT_REQUIRED_SIZE)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return TeLegacyNoUnwind(
      [&] { return TeProcessKey(context, key, out_view); });
}

int32_t FAMO_ENGINE_CALL TeSelectCandidateSafe(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeSelectCandidate(context, index, out_view); });
}

int32_t FAMO_ENGINE_CALL TeSetOptionSafe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t value) noexcept {
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return TeLegacyNoUnwind(
      [&] { return TeSetOption(context, name, value); });
}

int32_t FAMO_ENGINE_CALL TeDeploySchemaSafe(
    const FamoUtf8String* schema_id,
    FamoUtf8String* out_error_message) noexcept {
  if (!famo_action_v2::ValidInputString(schema_id))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return TeLegacyNoUnwind(
      [&] { return TeDeploySchema(schema_id, out_error_message); });
}

int32_t FAMO_ENGINE_CALL TeFreeViewSafe(
    FamoCompositionView* view) noexcept {
  return TeLegacyNoUnwind([&] { return TeFreeView(view); });
}

int32_t FAMO_ENGINE_CALL TeGetStatusSafe(
    FamoEngineContext* context, FamoCompositionView* out_view) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeGetStatus(context, out_view); });
}

int32_t FAMO_ENGINE_CALL TeGetOptionSafe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t* out_value) noexcept {
  if (out_value)
    *out_value = 0;
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return TeLegacyNoUnwind(
      [&] { return TeGetOption(context, name, out_value); });
}

int32_t FAMO_ENGINE_CALL TeCommitCompositionSafe(
    FamoEngineContext* context) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeCommitComposition(context); });
}

int32_t FAMO_ENGINE_CALL TeClearCompositionSafe(
    FamoEngineContext* context) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeClearComposition(context); });
}

int32_t FAMO_ENGINE_CALL TeHighlightCandidateSafe(
    FamoEngineContext* context, uint32_t index) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeHighlightCandidate(context, index); });
}

int32_t FAMO_ENGINE_CALL TeChangePageSafe(
    FamoEngineContext* context, int32_t backward) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TeChangePage(context, backward); });
}

int32_t FAMO_ENGINE_CALL TePeekCandidatesSafe(
    FamoEngineContext* context, uint32_t index, uint32_t count,
    FamoCompositionView* out_view) noexcept {
  return TeLegacyNoUnwind(
      [&] { return TePeekCandidates(context, index, count, out_view); });
}

int32_t FAMO_ENGINE_CALL TeSelectCandidateAbsoluteSafe(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) noexcept {
  return TeLegacyNoUnwind([&] {
    return TeSelectCandidateAbsolute(context, index, out_view);
  });
}

int32_t FAMO_ENGINE_CALL TeGetInfoV2Safe(
    FamoEngineInfo* out_info) noexcept {
  try {
    return TeGetInfoV2(out_info);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeInitializeV2Safe(
    const FamoEngineHostApi* host,
    const FamoUtf8String* data_root) noexcept {
  if (!famo_action_v2::ValidInputString(data_root))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return TeInitializeV2(host, data_root);
  } catch (...) {
    g_v2_results.Drain(g_host);
    std::memset(&g_host, 0, sizeof(g_host));
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeShutdownV2Safe() noexcept {
  try {
    return TeShutdown();
  } catch (...) {
    g_v2_results.Drain(g_host);
    std::memset(&g_host, 0, sizeof(g_host));
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeCreateContextV2Safe(
    const FamoUtf8String* schema_id,
    FamoEngineContext** out_context) noexcept {
  if (out_context)
    *out_context = nullptr;
  if (!famo_action_v2::ValidInputString(schema_id, true))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return TeCreateContext(schema_id, out_context);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeDestroyContextV2Safe(
    FamoEngineContext* context) noexcept {
  try {
    return TeDestroyContext(context);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeExecuteActionV2Safe(
    FamoEngineContext* context, const FamoEngineActionRequestV2* request,
    FamoEngineActionResultV2** out_result) noexcept {
  if (out_result)
    *out_result = nullptr;
  try {
    return TeExecuteActionV2(context, request, out_result);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeSetOptionV2Safe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t value) noexcept {
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return TeSetOption(context, name, value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeGetOptionV2Safe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t* out_value) noexcept {
  if (out_value)
    *out_value = 0;
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return TeGetOption(context, name, out_value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeDeploySchemaV2Safe(
    const FamoUtf8String* schema_id) noexcept {
  if (!famo_action_v2::ValidInputString(schema_id))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return TeDeploySchema(schema_id, nullptr);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeFreeResultV2Safe(
    FamoEngineActionResultV2* result) noexcept {
  try {
    return TeFreeResultV2(result);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeSetPropertyV2Safe(
    FamoEngineContext* context,
    const FamoUtf8String* name,
    const FamoUtf8String* value) noexcept {
  if (!famo_action_v2::ValidInputString(name) ||
      !famo_action_v2::ValidInputString(value)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  try {
    return TeSetPropertyV2(context, name, value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL TeSetNotificationHandlerV2Safe(
    FamoEngineNotificationHandlerV2 handler,
    void* user_data) noexcept {
  try {
    return TeSetNotificationHandlerV2(handler, user_data);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

}  // namespace

extern "C" FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL
FamoCreateEngineApi(uint32_t requested_abi_version, FamoEngineApi* out_api) {
  try {
  if (requested_abi_version != FAMO_ENGINE_ABI_VERSION)
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  if (!out_api)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const uint32_t caller_size = out_api->size;
  if (caller_size < static_cast<uint32_t>(offsetof(FamoEngineApi, get_status)))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;

  FamoEngineApi api{};
  api.size = caller_size < static_cast<uint32_t>(sizeof(api))
                 ? caller_size
                 : static_cast<uint32_t>(sizeof(api));
  api.abi_version = FAMO_ENGINE_ABI_VERSION;
  api.get_info = &TeGetInfoSafe;
  api.initialize = &TeInitializeSafe;
  api.shutdown = &TeShutdownSafe;
  api.create_context = &TeCreateContextSafe;
  api.destroy_context = &TeDestroyContextSafe;
  api.process_key = &TeProcessKeySafe;
  api.select_candidate = &TeSelectCandidateSafe;
  api.set_option = &TeSetOptionSafe;
  api.deploy_schema = &TeDeploySchemaSafe;
  api.free_view = &TeFreeViewSafe;
  api.get_status = &TeGetStatusSafe;
  api.get_option = &TeGetOptionSafe;
  api.commit_composition = &TeCommitCompositionSafe;
  api.clear_composition = &TeClearCompositionSafe;
  api.highlight_candidate = &TeHighlightCandidateSafe;
  api.change_page = &TeChangePageSafe;
  api.peek_candidates = &TePeekCandidatesSafe;
  api.select_candidate_absolute = &TeSelectCandidateAbsoluteSafe;
  std::memcpy(out_api, &api, api.size);
  return FAMO_ENGINE_OK;
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

extern "C" FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL
FamoCreateEngineApiV2(uint32_t requested_abi_version,
                      FamoEngineApiV2* out_api) {
  try {
  if (requested_abi_version != FAMO_ENGINE_ABI_V2)
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  if (!out_api)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const uint32_t caller_size = out_api->struct_size;
  if (caller_size <
      static_cast<uint32_t>(FAMO_ENGINE_API_V2_REQUIRED_SIZE))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;

  FamoEngineApiV2 api{};
  api.struct_size =
      caller_size < static_cast<uint32_t>(sizeof(api))
          ? caller_size
          : static_cast<uint32_t>(sizeof(api));
  api.abi_version = FAMO_ENGINE_ABI_V2;
  api.get_info = &TeGetInfoV2Safe;
  api.initialize = &TeInitializeV2Safe;
  api.shutdown = &TeShutdownV2Safe;
  api.create_context = &TeCreateContextV2Safe;
  api.destroy_context = &TeDestroyContextV2Safe;
  api.execute_action = &TeExecuteActionV2Safe;
  api.set_option = &TeSetOptionV2Safe;
  api.get_option = &TeGetOptionV2Safe;
  api.deploy_schema = &TeDeploySchemaV2Safe;
  api.free_result = &TeFreeResultV2Safe;
  api.set_property = &TeSetPropertyV2Safe;
  api.set_notification_handler = &TeSetNotificationHandlerV2Safe;
  std::memcpy(out_api, &api, api.struct_size);
  return FAMO_ENGINE_OK;
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}
