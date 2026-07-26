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
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include <windows.h>

#include "../../famo_engine_api.h"

namespace {

FamoEngineHostApi g_host;  // captured at initialize()

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
  auto it = Table().find(buffer);
  if (it != Table().end()) return it->second;
  return {buffer};  // echo unknown input as a single candidate
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
                    size_t count, FamoCompositionView* out) {
  if (!out || !g_host.alloc || start >= candidates.size() || count == 0)
    return;
  const size_t end = (std::min)(candidates.size(), start + count);
  const size_t size = end - start;
  auto* array = static_cast<FamoCandidate*>(
      g_host.alloc(sizeof(FamoCandidate) * size));
  if (!array)
    return;
  for (size_t i = 0; i < size; ++i) {
    std::memset(&array[i], 0, sizeof(FamoCandidate));
    array[i].size = static_cast<uint32_t>(sizeof(FamoCandidate));
    array[i].text = Dup(candidates[start + i]);
    array[i].comment = Dup("");
    array[i].flags = i == 0 ? FAMO_CANDIDATE_FLAG_DEFAULT : 0u;
    const char digit[2] = {
        static_cast<char>('0' + ((start + i + 1) % 10)), '\0'};
    array[i].label = Dup(digit);
  }
  out->candidates = array;
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
              FamoCompositionView* out) {
  std::memset(out, 0, sizeof(*out));
  out->size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  out->preedit = Dup(buffer);
  out->commit = Dup(commit);

  std::vector<std::string> cands = CandidatesFor(buffer);
  FillCandidates(cands, 0, cands.size(), out);
  const bool multipage = !Environment("FAMO_TEST_MULTIPAGE").empty() &&
                         out->candidate_count > 1;
  out->page_size = multipage ? 1u : out->candidate_count;

  uint32_t flags = 0;
  if (!buffer.empty()) flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (!commit.empty()) flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (out->candidate_count) flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  if (handled) flags |= FAMO_COMPOSITION_HANDLED;
  out->state_flags = flags;

  // v1.1 deterministic fields (exercise size negotiation + free_view teardown).
  out->preedit_sel_start = 0;
  out->preedit_sel_end = static_cast<uint32_t>(buffer.size());
  out->preedit_cursor_pos = static_cast<uint32_t>(buffer.size());
  out->commit_preview = Dup(buffer.empty() ? std::string() : CandidatesFor(buffer).front());
  out->schema_id = Dup("test");
  out->schema_name = Dup("Test Engine");
  out->status_flags =
      (Environment("FAMO_TEST_SIMPLIFIED").empty()
           ? 0u
           : static_cast<uint32_t>(FAMO_STATUS_SIMPLIFIED)) |
      (FAMO_STATUS_COMPOSING * (buffer.empty() ? 0u : 1u));
  out->is_last_page = multipage ? 0u : 1u;
}

}  // namespace

// Opaque context is a real struct inside the DLL; only UTF-8 crosses the ABI.
struct FamoEngineContext {
  std::string buffer;
  std::string pending_commit;
};

namespace {

int32_t FAMO_ENGINE_CALL TeGetInfo(FamoEngineInfo* out_info) {
  if (!out_info) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  out_info->size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  out_info->abi_version = FAMO_ENGINE_ABI_VERSION;
  out_info->capabilities = 0;  // deterministic stub: no Lua/OpenCC/userdb/deploy
  out_info->engine_name = Static("FamoTestEngine");
  out_info->engine_version = Static("1.0.0");
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeInitialize(const FamoEngineHostApi* host,
                                      const FamoUtf8String* /*data_root*/) {
  if (!host || host->size < static_cast<uint32_t>(sizeof(FamoEngineHostApi)) ||
      host->abi_version != FAMO_ENGINE_ABI_VERSION || !host->alloc || !host->free) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  g_host = *host;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeShutdown(void) {
  std::memset(&g_host, 0, sizeof(g_host));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeCreateContext(const FamoUtf8String* /*schema_id*/,
                                         FamoEngineContext** out_context) {
  if (!out_context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_context = new (std::nothrow) FamoEngineContext();
  return *out_context ? FAMO_ENGINE_OK : FAMO_ENGINE_E_RUNTIME;
}

int32_t FAMO_ENGINE_CALL TeDestroyContext(FamoEngineContext* context) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  delete context;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeProcessKey(FamoEngineContext* context,
                                      const FamoKeyEvent* key,
                                      FamoCompositionView* out_view) {
  if (!context || !key || !out_view) return FAMO_ENGINE_E_INVALID_ARGUMENT;

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
    FillView(context->buffer, "", handled, out_view);
    return FAMO_ENGINE_OK;
  }

  const uint32_t vk = key->virtual_key;
  if (vk == 0xff60 && !context->pending_commit.empty()) {
    std::string commit = std::move(context->pending_commit);
    FillView(context->buffer, commit, false, out_view);
    return FAMO_ENGINE_OK;
  }

  bool handled = false;
  if (vk >= 'a' && vk <= 'z') {
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
      FillView(context->buffer, "", false, out_view);
      return FAMO_ENGINE_OK;
    }
    std::vector<std::string> cands = CandidatesFor(context->buffer);
    std::string commit = cands.empty() ? context->buffer : cands[0];
    context->buffer.clear();
    FillView(context->buffer, commit, true, out_view);
    return FAMO_ENGINE_OK;
  }

  FillView(context->buffer, "", handled, out_view);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSelectCandidate(FamoEngineContext* context, uint32_t index,
                                           FamoCompositionView* out_view) {
  if (!context || !out_view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::vector<std::string> cands = CandidatesFor(context->buffer);
  if (index >= cands.size()) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::string commit = cands[index];
  context->buffer.clear();
  if (!Environment("FAMO_TEST_DEFER_SELECTION_COMMIT").empty()) {
    context->pending_commit = std::move(commit);
    FillView(context->buffer, "", true, out_view);
    return FAMO_ENGINE_OK;
  }
  FillView(context->buffer, commit, true, out_view);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeSetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name, int32_t /*value*/) {
  if (!context || !name) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string fail = Environment("FAMO_TEST_FAIL_OPTION");
  const std::string_view option(name->data ? name->data : "", name->length_bytes);
  if (!fail.empty() && option == fail) return FAMO_ENGINE_E_RUNTIME;
  return FAMO_ENGINE_OK;  // stub: options accepted, no effect
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
  if (!view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  HostFreeStr(&view->preedit);
  HostFreeStr(&view->commit);
  if (view->candidates && g_host.free) {
    auto* arr = const_cast<FamoCandidate*>(view->candidates);
    for (uint32_t i = 0; i < view->candidate_count; ++i) {
      HostFreeStr(&arr[i].text);
      HostFreeStr(&arr[i].comment);
      HostFreeStr(&arr[i].label);  // v1.2
    }
    g_host.free(arr);
  }
  // v1.1 strings: only touch them if the caller's struct actually spans them.
  if (view->size >= offsetof(FamoCompositionView, status_flags)) {
    HostFreeStr(&view->commit_preview);
    HostFreeStr(&view->schema_id);
    HostFreeStr(&view->schema_name);
  }
  std::memset(view, 0, sizeof(*view));
  return FAMO_ENGINE_OK;
}

// v1.1 deterministic stubs.
int32_t FAMO_ENGINE_CALL TeGetStatus(FamoEngineContext* context,
                                     FamoCompositionView* out_view) {
  if (!context || !out_view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  FillView(context->buffer, "", false, out_view);  // status folded in; no commit
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL TeGetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name, int32_t* out_value) {
  if (!context || !name || !out_value) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_value = 0;  // stub: every option reads back as off
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
  if (!context || !out_view || count > 64)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::memset(out_view, 0, sizeof(*out_view));
  out_view->size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  FillCandidates(CandidatesFor(context->buffer), index, count, out_view);
  return FAMO_ENGINE_OK;
}

}  // namespace

extern "C" FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL
FamoCreateEngineApi(uint32_t requested_abi_version, FamoEngineApi* out_api) {
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
  api.get_info = &TeGetInfo;
  api.initialize = &TeInitialize;
  api.shutdown = &TeShutdown;
  api.create_context = &TeCreateContext;
  api.destroy_context = &TeDestroyContext;
  api.process_key = &TeProcessKey;
  api.select_candidate = &TeSelectCandidate;
  api.set_option = &TeSetOption;
  api.deploy_schema = &TeDeploySchema;
  api.free_view = &TeFreeView;
  api.get_status = &TeGetStatus;
  api.get_option = &TeGetOption;
  api.commit_composition = &TeCommitComposition;
  api.clear_composition = &TeClearComposition;
  api.highlight_candidate = &TeHighlightCandidate;
  api.change_page = &TeChangePage;
  api.peek_candidates = &TePeekCandidates;
  std::memcpy(out_api, &api, api.size);
  return FAMO_ENGINE_OK;
}
