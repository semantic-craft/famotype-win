#pragma once

#include <stddef.h>
#include <stdint.h>

#define FAMO_ENGINE_ABI_VERSION 1u
// ABI minor: signalled by struct size, not by the major version. v1.1 appends
// fields to FamoCompositionView and functions to FamoEngineApi; both are
// size-negotiated, so a v1.0 host/engine and a v1.1 one interoperate at v1.0.
// v1.2 appends FamoCandidate::label and FamoCompositionView::is_last_page (the
// FamoCandidateUI render inputs); v1.3 appends peek_candidates. Both follow the
// same append-only/size-negotiated discipline.
#define FAMO_ENGINE_ABI_MINOR 3u

#if defined(_WIN32)
#define FAMO_ENGINE_CALL __stdcall
#define FAMO_ENGINE_EXPORT __declspec(dllexport)
#else
#define FAMO_ENGINE_CALL
#define FAMO_ENGINE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FamoEngineContext FamoEngineContext;

typedef enum FamoEngineResult {
  FAMO_ENGINE_OK = 0,
  FAMO_ENGINE_E_INVALID_ARGUMENT = 0x00010001,
  FAMO_ENGINE_E_UNSUPPORTED_ABI = 0x00010002,
  FAMO_ENGINE_E_RUNTIME = 0x00020001,
  FAMO_ENGINE_E_SCHEMA = 0x00030001,
  FAMO_ENGINE_E_DICT = 0x00040001,
  FAMO_ENGINE_E_USERDB = 0x00050001,
  FAMO_ENGINE_E_IPC = 0x00060001
} FamoEngineResult;

typedef enum FamoEngineLogLevel {
  FAMO_ENGINE_LOG_TRACE = 0,
  FAMO_ENGINE_LOG_INFO = 1,
  FAMO_ENGINE_LOG_WARNING = 2,
  FAMO_ENGINE_LOG_ERROR = 3
} FamoEngineLogLevel;

#define FAMO_ENGINE_CAP_LUA (1ull << 0)
#define FAMO_ENGINE_CAP_OPENCC (1ull << 1)
#define FAMO_ENGINE_CAP_USERDB_SYNC (1ull << 2)
#define FAMO_ENGINE_CAP_SCHEMA_DEPLOY (1ull << 3)
#define FAMO_ENGINE_CAP_PREDICTION (1ull << 4)

#define FAMO_KEY_MOD_SHIFT (1u << 0)
#define FAMO_KEY_MOD_CONTROL (1u << 1)
#define FAMO_KEY_MOD_ALT (1u << 2)
#define FAMO_KEY_MOD_SUPER (1u << 3)
#define FAMO_KEY_MOD_CAPS_LOCK (1u << 4)

#define FAMO_CANDIDATE_FLAG_DEFAULT (1u << 0)
#define FAMO_COMPOSITION_HAS_PREEDIT (1u << 0)
#define FAMO_COMPOSITION_HAS_COMMIT (1u << 1)
#define FAMO_COMPOSITION_HAS_CANDIDATES (1u << 2)
// Exact result of the operation that produced this view. In particular,
// process_key must copy librime's bool here; callers never infer from preedit.
#define FAMO_COMPOSITION_HANDLED (1u << 3)

// v1.1 status bits carried in FamoCompositionView::status_flags (distinct from
// state_flags above). Simplified is derived from the supported schemas'
// traditionalization options; the other bits mirror RimeStatus booleans.
#define FAMO_STATUS_ASCII_MODE (1u << 0)
#define FAMO_STATUS_COMPOSING (1u << 1)
#define FAMO_STATUS_DISABLED (1u << 2)
#define FAMO_STATUS_FULL_SHAPE (1u << 3)
#define FAMO_STATUS_ASCII_PUNCT (1u << 4)
#define FAMO_STATUS_SIMPLIFIED (1u << 5)

typedef struct FamoUtf8String {
  uint32_t size;
  const char* data;
  uint32_t length_bytes;
} FamoUtf8String;

typedef struct FamoEngineHostApi {
  uint32_t size;
  uint32_t abi_version;
  void* (FAMO_ENGINE_CALL *alloc)(size_t bytes);
  void (FAMO_ENGINE_CALL *free)(void* p);
  void (FAMO_ENGINE_CALL *log)(int32_t level,
                               const FamoUtf8String* domain,
                               const FamoUtf8String* message);
} FamoEngineHostApi;

typedef struct FamoEngineInfo {
  uint32_t size;
  uint32_t abi_version;
  uint64_t capabilities;
  FamoUtf8String engine_name;
  FamoUtf8String engine_version;
} FamoEngineInfo;

typedef struct FamoKeyEvent {
  uint32_t size;
  uint32_t virtual_key;
  uint32_t scan_code;
  uint32_t modifiers;
  uint32_t is_key_down;
  uint64_t timestamp_ms;
} FamoKeyEvent;

typedef struct FamoCandidate {
  uint32_t size;
  FamoUtf8String text;
  FamoUtf8String comment;
  uint32_t quality;
  uint32_t flags;
  // ── v1.2 addition (append only) ──────────────────────────────────────────
  FamoUtf8String label;  // select label/key: select_labels -> select_keys -> (i+1)%10
} FamoCandidate;

typedef struct FamoCompositionView {
  uint32_t size;
  FamoUtf8String preedit;
  FamoUtf8String commit;
  const FamoCandidate* candidates;
  uint32_t candidate_count;
  uint32_t highlighted_index;
  uint32_t page_index;
  uint32_t page_size;
  uint32_t state_flags;
  // ── v1.1 additions (append only; guard reads on view.size) ──────────────
  uint32_t preedit_sel_start;     // byte offset of selection start in preedit
  uint32_t preedit_sel_end;       // byte offset of selection end in preedit
  uint32_t preedit_cursor_pos;    // byte offset of caret in preedit
  FamoUtf8String commit_preview;  // inline commit preview (ctx.commit_text_preview)
  FamoUtf8String schema_id;       // folded status: current schema id
  FamoUtf8String schema_name;     // current schema display name
  uint32_t status_flags;          // FAMO_STATUS_* bits
  // ── v1.2 addition (append only; guard reads on view.size) ────────────────
  uint32_t is_last_page;          // menu.is_last_page — disambiguates a full last page
} FamoCompositionView;

typedef struct FamoEngineApi {
  uint32_t size;
  uint32_t abi_version;
  int32_t (FAMO_ENGINE_CALL *get_info)(FamoEngineInfo* out_info);
  int32_t (FAMO_ENGINE_CALL *initialize)(const FamoEngineHostApi* host,
                                         const FamoUtf8String* data_root);
  int32_t (FAMO_ENGINE_CALL *shutdown)(void);
  int32_t (FAMO_ENGINE_CALL *create_context)(
      const FamoUtf8String* schema_id,
      FamoEngineContext** out_context);
  int32_t (FAMO_ENGINE_CALL *destroy_context)(FamoEngineContext* context);
  int32_t (FAMO_ENGINE_CALL *process_key)(FamoEngineContext* context,
                                          const FamoKeyEvent* key,
                                          FamoCompositionView* out_view);
  int32_t (FAMO_ENGINE_CALL *select_candidate)(FamoEngineContext* context,
                                               uint32_t index,
                                               FamoCompositionView* out_view);
  int32_t (FAMO_ENGINE_CALL *set_option)(FamoEngineContext* context,
                                         const FamoUtf8String* name,
                                         int32_t value);
  int32_t (FAMO_ENGINE_CALL *deploy_schema)(
      const FamoUtf8String* schema_id,
      FamoUtf8String* out_error_message);
  int32_t (FAMO_ENGINE_CALL *free_view)(FamoCompositionView* view);
  // ── v1.1 additions (append only; callable iff size covers the offset) ────
  // Fill out_view with current status + context (no commit consume). Same view
  // shape and free_view teardown as process_key/select_candidate.
  int32_t (FAMO_ENGINE_CALL *get_status)(FamoEngineContext* context,
                                         FamoCompositionView* out_view);
  // Read a boolean runtime option (e.g. ascii_mode, vim_mode) into *out_value.
  int32_t (FAMO_ENGINE_CALL *get_option)(FamoEngineContext* context,
                                         const FamoUtf8String* name,
                                         int32_t* out_value);
  // Thin session ops: one-line librime delegations the runtime reroute needs.
  int32_t (FAMO_ENGINE_CALL *commit_composition)(FamoEngineContext* context);
  int32_t (FAMO_ENGINE_CALL *clear_composition)(FamoEngineContext* context);
  int32_t (FAMO_ENGINE_CALL *highlight_candidate)(FamoEngineContext* context,
                                                  uint32_t index);
  int32_t (FAMO_ENGINE_CALL *change_page)(FamoEngineContext* context,
                                          int32_t backward);
  // ── v1.3 optional addition (append only; callable iff size covers it) ────
  // Read up to count candidates from the absolute list index without changing
  // the current page/highlight. The returned view owns only its candidate array
  // and is released through free_view. Missing support degrades to no preview.
  int32_t (FAMO_ENGINE_CALL *peek_candidates)(FamoEngineContext* context,
                                               uint32_t index,
                                               uint32_t count,
                                               FamoCompositionView* out_view);
} FamoEngineApi;

FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL FamoCreateEngineApi(
    uint32_t requested_abi_version,
    FamoEngineApi* out_api);

#ifdef __cplusplus
}
#endif
