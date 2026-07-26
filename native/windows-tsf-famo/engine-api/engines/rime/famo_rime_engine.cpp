// FamoRimeEngine.dll - librime adapter for FamoEngineApi v1.
//
// Implements the engine ABI by delegating to the RimeApi function table
// (rime_get_api()), linking librime (rime.lib). No RIME struct escapes the DLL:
// RIME output is copied into host-allocated UTF-8 and the RIME structs are freed
// immediately; the host later releases the view via free_view().
//
// Behavioral correctness of candidates depends on deployed schemas/dicts and is
// verified on the target machine (see task prd/design). On this machine the
// smoke proves build+link+load+init+deploy+roundtrip without crashing.
#include <cstdint>
#include <cstring>
#include <string>

#include "../../famo_engine_api.h"
#include "keymap.h"

#include <rime_api.h>

namespace {

FamoEngineHostApi g_host;
RimeApi* g_rime = nullptr;

FamoUtf8String DupN(const char* s, size_t n) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = nullptr;
  v.length_bytes = 0;
  if (!g_host.alloc) return v;
  char* p = static_cast<char*>(g_host.alloc(n + 1));
  if (!p) return v;
  if (n) std::memcpy(p, s, n);
  p[n] = '\0';
  v.data = p;
  v.length_bytes = static_cast<uint32_t>(n);
  return v;
}

FamoUtf8String DupC(const char* s) { return DupN(s ? s : "", s ? std::strlen(s) : 0); }

FamoUtf8String Static(const char* s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = static_cast<uint32_t>(std::strlen(s));
  return v;
}

void FreeStr(FamoUtf8String* v) {
  if (v && v->data && g_host.free) {
    g_host.free(const_cast<char*>(v->data));
    v->data = nullptr;
    v->length_bytes = 0;
  }
}

std::string AsStd(const FamoUtf8String* s) {
  if (s && s->data) return std::string(s->data, s->length_bytes);
  return std::string();
}

inline uint32_t NonNeg(int v) { return static_cast<uint32_t>(v < 0 ? 0 : v); }

// v1.1: fold RimeStatus into the view (schema id/name + status_flags). No commit
// consume; safe to call from get_status and from the per-key fill alike.
void FillStatus(RimeSessionId session, FamoCompositionView* out) {
  RIME_STRUCT(RimeStatus, status);
  if (!g_rime->get_status(session, &status)) return;
  out->schema_id = DupC(status.schema_id);
  out->schema_name = DupC(status.schema_name);
  uint32_t f = 0;
  if (status.is_ascii_mode) f |= FAMO_STATUS_ASCII_MODE;
  if (status.is_composing) f |= FAMO_STATUS_COMPOSING;
  if (status.is_disabled) f |= FAMO_STATUS_DISABLED;
  if (status.is_full_shape) f |= FAMO_STATUS_FULL_SHAPE;
  if (status.is_ascii_punct) f |= FAMO_STATUS_ASCII_PUNCT;
  const bool traditional =
      g_rime->get_option(session, "traditionalization") ||
      g_rime->get_option(session, "zh_trad");
  if (!traditional) f |= FAMO_STATUS_SIMPLIFIED;
  out->status_flags = f;
  g_rime->free_status(&status);
}

// Read the session's current context (+ status) into a host-owned view.
// consume_commit: pull RimeCommit (true on the key hot path, false for a pure
// status read that must not swallow pending commit text).
int32_t FillFromSession(RimeSessionId session, FamoCompositionView* out,
                        bool consume_commit) {
  std::memset(out, 0, sizeof(*out));
  out->size = static_cast<uint32_t>(sizeof(FamoCompositionView));

  if (consume_commit) {
    RIME_STRUCT(RimeCommit, commit);
    if (g_rime->get_commit(session, &commit)) {
      if (commit.text) out->commit = DupC(commit.text);
      g_rime->free_commit(&commit);
    }
  }

  RIME_STRUCT(RimeContext, ctx);
  if (g_rime->get_context(session, &ctx)) {
    if (ctx.composition.preedit) out->preedit = DupC(ctx.composition.preedit);
    out->preedit_sel_start = NonNeg(ctx.composition.sel_start);
    out->preedit_sel_end = NonNeg(ctx.composition.sel_end);
    out->preedit_cursor_pos = NonNeg(ctx.composition.cursor_pos);
    if (ctx.commit_text_preview) out->commit_preview = DupC(ctx.commit_text_preview);
    const int n = ctx.menu.num_candidates;
    if (n > 0 && g_host.alloc) {
      auto* arr = static_cast<FamoCandidate*>(g_host.alloc(sizeof(FamoCandidate) * n));
      if (arr) {
        // Length of the page's select-key string, hoisted so the per-candidate
        // fallback below indexes it in-bounds (a misconfigured schema can define
        // fewer select_keys than candidates on the page).
        const size_t n_keys =
            ctx.menu.select_keys ? std::strlen(ctx.menu.select_keys) : 0;
        for (int i = 0; i < n; ++i) {
          std::memset(&arr[i], 0, sizeof(FamoCandidate));
          arr[i].size = static_cast<uint32_t>(sizeof(FamoCandidate));
          arr[i].text = DupC(ctx.menu.candidates[i].text);
          arr[i].comment = DupC(ctx.menu.candidates[i].comment);
          arr[i].flags = (i == ctx.menu.highlighted_candidate_index)
                             ? FAMO_CANDIDATE_FLAG_DEFAULT
                             : 0u;
          // v1.2 label: per-candidate select_labels -> menu.select_keys[i] ->
          // (i+1)%10. select_labels/select_keys index the *page*, so use i.
          if (ctx.select_labels && ctx.select_labels[i]) {
            arr[i].label = DupC(ctx.select_labels[i]);
          } else if (static_cast<size_t>(i) < n_keys) {
            const char key[2] = {ctx.menu.select_keys[i], '\0'};
            arr[i].label = DupC(key);
          } else {
            const char digit[2] = {static_cast<char>('0' + (i + 1) % 10), '\0'};
            arr[i].label = DupC(digit);
          }
        }
        out->candidates = arr;
        out->candidate_count = static_cast<uint32_t>(n);
      }
    }
    out->highlighted_index =
        static_cast<uint32_t>(ctx.menu.highlighted_candidate_index < 0
                                  ? 0
                                  : ctx.menu.highlighted_candidate_index);
    out->page_index = static_cast<uint32_t>(ctx.menu.page_no < 0 ? 0 : ctx.menu.page_no);
    out->page_size = static_cast<uint32_t>(ctx.menu.page_size < 0 ? 0 : ctx.menu.page_size);
    out->is_last_page = ctx.menu.is_last_page ? 1u : 0u;  // v1.2
    g_rime->free_context(&ctx);
  }

  FillStatus(session, out);

  uint32_t flags = 0;
  if (out->preedit.length_bytes) flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (out->commit.length_bytes) flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (out->candidate_count) flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  out->state_flags = flags;
  return FAMO_ENGINE_OK;
}

}  // namespace

// Opaque context: a RIME session. Only UTF-8 crosses the ABI.
struct FamoEngineContext {
  RimeSessionId session;
};

namespace {

int32_t FAMO_ENGINE_CALL ReGetInfo(FamoEngineInfo* out_info) {
  if (!out_info) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  out_info->size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  out_info->abi_version = FAMO_ENGINE_ABI_VERSION;
  // Conservative/honest: this build reliably supports deploy + userdb sync.
  // Lua/OpenCC depend on the librime build and are left unadvertised for MVP.
  out_info->capabilities = FAMO_ENGINE_CAP_SCHEMA_DEPLOY | FAMO_ENGINE_CAP_USERDB_SYNC;
  out_info->engine_name = Static("FamoRimeEngine");
  out_info->engine_version = Static("1.0.0");
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReInitialize(const FamoEngineHostApi* host,
                                      const FamoUtf8String* data_root) {
  if (!host || host->size < static_cast<uint32_t>(sizeof(FamoEngineHostApi)) ||
      host->abi_version != FAMO_ENGINE_ABI_VERSION || !host->alloc || !host->free) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  g_host = *host;
  g_rime = rime_get_api();
  if (!g_rime) return FAMO_ENGINE_E_RUNTIME;

  const std::string root = AsStd(data_root);

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = root.c_str();
  traits.user_data_dir = root.c_str();
  traits.distribution_name = "Famo";
  traits.distribution_code_name = "FamoRimeEngine";
  traits.distribution_version = "1.0.0";
  traits.app_name = "rime.famo";
  traits.min_log_level = 2;  // ERROR
  traits.log_dir = "";       // stderr only

  g_rime->setup(&traits);       // copies traits internally
  g_rime->initialize(&traits);  // ditto; pointers valid for the call duration
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReShutdown(void) {
  if (g_rime) g_rime->finalize();
  g_rime = nullptr;
  std::memset(&g_host, 0, sizeof(g_host));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReCreateContext(const FamoUtf8String* schema_id,
                                         FamoEngineContext** out_context) {
  if (!out_context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  RimeSessionId session = g_rime->create_session();
  if (!session) return FAMO_ENGINE_E_RUNTIME;
  const std::string schema = AsStd(schema_id);
  if (!schema.empty()) g_rime->select_schema(session, schema.c_str());
  auto* ctx = new (std::nothrow) FamoEngineContext();
  if (!ctx) {
    g_rime->destroy_session(session);
    return FAMO_ENGINE_E_RUNTIME;
  }
  ctx->session = session;
  *out_context = ctx;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReDestroyContext(FamoEngineContext* context) {
  if (!context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (g_rime) g_rime->destroy_session(context->session);
  delete context;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReProcessKey(FamoEngineContext* context, const FamoKeyEvent* key,
                                      FamoCompositionView* out_view) {
  if (!context || !key || !out_view || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  int keycode = 0, mask = 0;
  bool handled = false;
  if (famo_rime_keys::FamoKeyToRime(*key, &keycode, &mask))
    handled = g_rime->process_key(context->session, keycode, mask);
  const int32_t rc = FillFromSession(context->session, out_view, /*consume_commit=*/true);
  if (rc == FAMO_ENGINE_OK && handled)
    out_view->state_flags |= FAMO_COMPOSITION_HANDLED;
  return rc;
}

int32_t FAMO_ENGINE_CALL ReSelectCandidate(FamoEngineContext* context, uint32_t index,
                                           FamoCompositionView* out_view) {
  if (!context || !out_view || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const bool handled = g_rime->select_candidate_on_current_page(
      context->session, static_cast<size_t>(index));
  // consume_commit=false, legacy-faithful: the reroute's SelectCandidateOnCurrentPage does
  // NOT _Respond (mirrors stock weasel — the IPC On-handler passes no eat). The commit the
  // selection produces is delivered by the FOLLOWING simulated VK_SELECT key
  // (CandidateList.cpp _SelectCandidateOnCurrentPage) via ProcessKeyEvent→_Respond→get_commit.
  // Consuming it here strands it in a view that SelectCandidateOnCurrentPage discards →
  // mouse-clicking a candidate drops the committed text under abi (byte-parity harness caught this).
  const int32_t rc = FillFromSession(context->session, out_view, /*consume_commit=*/false);
  if (rc == FAMO_ENGINE_OK && handled)
    out_view->state_flags |= FAMO_COMPOSITION_HANDLED;
  return rc;
}

int32_t FAMO_ENGINE_CALL ReSetOption(FamoEngineContext* context, const FamoUtf8String* name,
                                     int32_t value) {
  if (!context || !name || !name->data || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string opt = AsStd(name);
  g_rime->set_option(context->session, opt.c_str(), value ? True : False);
  if (value && (opt == "traditionalization" || opt == "zh_trad")) {
    // Load OpenCC off the 50 ms TSF key path without disturbing active input.
    const char* input = g_rime->get_input(context->session);
    if (!input || !*input) {
      g_rime->process_key(context->session, 'a', 0);
      g_rime->clear_composition(context->session);
    }
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReDeploySchema(const FamoUtf8String* /*schema_id*/,
                                        FamoUtf8String* /*out_error_message*/) {
  if (!g_rime) return FAMO_ENGINE_E_RUNTIME;
  // MVP: run a full maintenance pass (compiles bundled schemas) and block until
  // done. Empty data root -> nothing to build, returns promptly.
  Bool started = g_rime->start_maintenance(True);
  if (started) g_rime->join_maintenance_thread();
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReFreeView(FamoCompositionView* view) {
  if (!view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  FreeStr(&view->preedit);
  FreeStr(&view->commit);
  if (view->candidates && g_host.free) {
    auto* arr = const_cast<FamoCandidate*>(view->candidates);
    for (uint32_t i = 0; i < view->candidate_count; ++i) {
      FreeStr(&arr[i].text);
      FreeStr(&arr[i].comment);
      FreeStr(&arr[i].label);  // v1.2
    }
    g_host.free(arr);
  }
  // v1.1 strings: only touch them if the caller's struct actually spans them.
  if (view->size >= offsetof(FamoCompositionView, status_flags)) {
    FreeStr(&view->commit_preview);
    FreeStr(&view->schema_id);
    FreeStr(&view->schema_name);
  }
  std::memset(view, 0, sizeof(*view));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReGetStatus(FamoEngineContext* context,
                                     FamoCompositionView* out_view) {
  if (!context || !out_view || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return FillFromSession(context->session, out_view, /*consume_commit=*/false);
}

int32_t FAMO_ENGINE_CALL ReGetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name, int32_t* out_value) {
  if (!context || !name || !name->data || !out_value || !g_rime)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string opt = AsStd(name);
  *out_value = g_rime->get_option(context->session, opt.c_str()) ? 1 : 0;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReCommitComposition(FamoEngineContext* context) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->commit_composition(context->session);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReClearComposition(FamoEngineContext* context) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->clear_composition(context->session);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReHighlightCandidate(FamoEngineContext* context, uint32_t index) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->highlight_candidate_on_current_page(context->session, static_cast<size_t>(index));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReChangePage(FamoEngineContext* context, int32_t backward) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->change_page(context->session, backward ? True : False);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL RePeekCandidates(FamoEngineContext* context,
                                          uint32_t index, uint32_t count,
                                          FamoCompositionView* out_view) {
  if (!context || !out_view || !g_rime || count > 64)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  std::memset(out_view, 0, sizeof(*out_view));
  out_view->size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  if (count == 0 || !RIME_API_AVAILABLE(g_rime, candidate_list_from_index) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_next) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_end))
    return FAMO_ENGINE_OK;

  RimeCandidateListIterator iterator{};
  if (!g_rime->candidate_list_from_index(context->session, &iterator,
                                          static_cast<int>(index)))
    return FAMO_ENGINE_OK;
  auto* candidates = static_cast<FamoCandidate*>(
      g_host.alloc(sizeof(FamoCandidate) * count));
  if (!candidates) {
    g_rime->candidate_list_end(&iterator);
    return FAMO_ENGINE_E_RUNTIME;
  }

  uint32_t size = 0;
  while (size < count && g_rime->candidate_list_next(&iterator)) {
    FamoCandidate& candidate = candidates[size];
    std::memset(&candidate, 0, sizeof(candidate));
    candidate.size = static_cast<uint32_t>(sizeof(FamoCandidate));
    candidate.text = DupC(iterator.candidate.text);
    candidate.comment = DupC(iterator.candidate.comment);
    const char digit[2] = {
        static_cast<char>('0' + ((index + size + 1) % 10)), '\0'};
    candidate.label = DupC(digit);
    ++size;
  }
  g_rime->candidate_list_end(&iterator);
  if (size == 0) {
    g_host.free(candidates);
    return FAMO_ENGINE_OK;
  }
  out_view->candidates = candidates;
  out_view->candidate_count = size;
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
  api.get_info = &ReGetInfo;
  api.initialize = &ReInitialize;
  api.shutdown = &ReShutdown;
  api.create_context = &ReCreateContext;
  api.destroy_context = &ReDestroyContext;
  api.process_key = &ReProcessKey;
  api.select_candidate = &ReSelectCandidate;
  api.set_option = &ReSetOption;
  api.deploy_schema = &ReDeploySchema;
  api.free_view = &ReFreeView;
  api.get_status = &ReGetStatus;
  api.get_option = &ReGetOption;
  api.commit_composition = &ReCommitComposition;
  api.clear_composition = &ReClearComposition;
  api.highlight_candidate = &ReHighlightCandidate;
  api.change_page = &ReChangePage;
  api.peek_candidates = &RePeekCandidates;
  std::memcpy(out_api, &api, api.size);
  return FAMO_ENGINE_OK;
}
