#pragma once

#include <stddef.h>
#include <stdint.h>

#define FAMO_ENGINE_ABI_VERSION 1u
// ABI minor: signalled by struct size, not by the major version. v1.1 appends
// fields to FamoCompositionView and functions to FamoEngineApi; both are
// size-negotiated, so a v1.0 host/engine and a v1.1 one interoperate at v1.0.
// v1.2 appends FamoCandidate::label and FamoCompositionView::is_last_page (the
// FamoCandidateUI render inputs); v1.3 appends peek_candidates and v1.4 appends
// absolute candidate selection. All follow the same append-only discipline.
#define FAMO_ENGINE_ABI_MINOR 4u

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
  // The context has one already-dispatched v2 action whose final snapshot must
  // be recovered before another business action can be accepted.
  FAMO_ENGINE_E_RECOVERY_REQUIRED = 0x00020002,
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

// Expanded librime/IBus mask bits. FamoKeyEvent::modifiers is passed through to
// librime, so these values deliberately match its public key-event contract.
#define FAMO_KEY_MOD_SHIFT (1u << 0)
#define FAMO_KEY_MOD_CAPS_LOCK (1u << 1)
#define FAMO_KEY_MOD_CONTROL (1u << 2)
#define FAMO_KEY_MOD_ALT (1u << 3)
#define FAMO_KEY_MOD_SUPER (1u << 26)
#define FAMO_KEY_MOD_RELEASE (1u << 30)
#ifdef __cplusplus
static_assert(FAMO_KEY_MOD_SHIFT == 0x00000001u &&
                  FAMO_KEY_MOD_CAPS_LOCK == 0x00000002u &&
                  FAMO_KEY_MOD_CONTROL == 0x00000004u &&
                  FAMO_KEY_MOD_ALT == 0x00000008u &&
                  FAMO_KEY_MOD_SUPER == 0x04000000u &&
                  FAMO_KEY_MOD_RELEASE == 0x40000000u,
              "key modifier constants must match librime's expanded mask");
#endif

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

#define FAMO_UTF8_STRING_REQUIRED_SIZE \
  (offsetof(FamoUtf8String, length_bytes) + sizeof(uint32_t))

typedef struct FamoEngineHostApi {
  uint32_t size;
  uint32_t abi_version;
  // Host callbacks are no-throw. No C++ exception or SEH unwind may cross
  // either direction of this C ABI boundary.
  void* (FAMO_ENGINE_CALL *alloc)(size_t bytes);
  void (FAMO_ENGINE_CALL *free)(void* p);
  void (FAMO_ENGINE_CALL *log)(int32_t level,
                               const FamoUtf8String* domain,
                               const FamoUtf8String* message);
} FamoEngineHostApi;

#define FAMO_ENGINE_HOST_API_REQUIRED_SIZE \
  (offsetof(FamoEngineHostApi, log) +       \
   sizeof(((FamoEngineHostApi*)0)->log))
#ifdef __cplusplus
static_assert(FAMO_ENGINE_HOST_API_REQUIRED_SIZE <=
                  sizeof(FamoEngineHostApi),
              "host API field span exceeds its native structure");
#endif

typedef struct FamoEngineInfo {
  uint32_t size;
  uint32_t abi_version;
  uint64_t capabilities;
  // Borrowed static strings, valid until engine shutdown; never free them.
  FamoUtf8String engine_name;
  FamoUtf8String engine_version;
} FamoEngineInfo;

#define FAMO_ENGINE_INFO_REQUIRED_SIZE \
  (offsetof(FamoEngineInfo, engine_version) + sizeof(FamoUtf8String))

typedef struct FamoKeyEvent {
  uint32_t size;
  // A librime/X11 keysym, not a Windows virtual-key code.
  uint32_t virtual_key;
  uint32_t scan_code;
  // Expanded librime mask. RELEASE must agree with is_key_down.
  uint32_t modifiers;
  uint32_t is_key_down;
  uint64_t timestamp_ms;
} FamoKeyEvent;

#define FAMO_KEY_EVENT_REQUIRED_SIZE \
  (offsetof(FamoKeyEvent, timestamp_ms) + sizeof(uint64_t))

typedef struct FamoCandidate {
  uint32_t size;
  FamoUtf8String text;
  FamoUtf8String comment;
  uint32_t quality;
  uint32_t flags;
  // ── v1.2 addition (append only) ──────────────────────────────────────────
  FamoUtf8String label;  // select label/key: select_labels -> select_keys -> (i+1)%10
} FamoCandidate;

// The size member is the caller-owned writable span. Set it before every query.
// The engine rejects a span smaller than the v1.0 prefix, writes at most this
// many bytes, and preserves the negotiated value across free_view for reuse.
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

// Stable field-span boundaries for FamoCompositionView negotiation. Use field
// ends rather than sizeof an older source definition: tail padding is not a
// version marker on every architecture.
#define FAMO_COMPOSITION_VIEW_V1_REQUIRED_SIZE \
  offsetof(FamoCompositionView, preedit_sel_start)
#define FAMO_COMPOSITION_VIEW_V11_FIELD_SIZE \
  offsetof(FamoCompositionView, is_last_page)
#define FAMO_COMPOSITION_VIEW_V12_FIELD_SIZE \
  (offsetof(FamoCompositionView, is_last_page) + sizeof(uint32_t))

// Before v1.2, candidate arrays are packed with this stride and contain no
// label. A view spanning the complete v1.2 is_last_page field uses the current
// sizeof(FamoCandidate) stride. Engines must use the same derived stride for
// allocation, indexing, and free_view.
#define FAMO_CANDIDATE_V1_STRIDE offsetof(FamoCandidate, label)

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
  // ── v1.4 optional addition (append only; callable iff size covers it) ────
  int32_t (FAMO_ENGINE_CALL *select_candidate_absolute)(
      FamoEngineContext* context,
      uint32_t index,
      FamoCompositionView* out_view);
} FamoEngineApi;

// ── ABI v2: one action in, one engine-owned result out ─────────────────────
//
// v1 remains frozen above for binary compatibility. v2 is a separate table,
// view, and candidate layout negotiated through a separate export; no table or
// structure may be cast between the two ABIs.
#define FAMO_ENGINE_ABI_V2 2u
#define FAMO_COMPOSITION_LAYOUT_V2 2u
#define FAMO_CANDIDATE_LAYOUT_V2 2u
#define FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES 64u
// Hard allocation contract shared by every v2 engine and consumer. Ordinary
// page views are not limited by the PEEK count; they are bounded by this
// independent candidate/UTF-8/result budget instead.
#define FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES 4096u
#define FAMO_ENGINE_V2_MAX_STRING_BYTES (1024u * 1024u)
#define FAMO_ENGINE_V2_MAX_RESULT_BYTES (8u * 1024u * 1024u)

typedef enum FamoEngineActionV2 {
  FAMO_ENGINE_ACTION_PROCESS_KEY = 1,
  FAMO_ENGINE_ACTION_SELECT_CANDIDATE = 2,
  FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE = 3,
  FAMO_ENGINE_ACTION_COMMIT_COMPOSITION = 4,
  FAMO_ENGINE_ACTION_CLEAR_COMPOSITION = 5,
  FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE = 6,
  FAMO_ENGINE_ACTION_CHANGE_PAGE = 7,
  FAMO_ENGINE_ACTION_STATUS = 8,
  FAMO_ENGINE_ACTION_PEEK_CANDIDATES = 9,
  // Control action only: value is the original business action. RECOVER never
  // mutates engine state and its returned result action remains the original
  // business action so that one dispatch still has one final formatted result.
  FAMO_ENGINE_ACTION_RECOVER = 10
} FamoEngineActionV2;

typedef struct FamoCandidateV2 {
  uint32_t struct_size;
  FamoUtf8String text;
  FamoUtf8String comment;
  uint32_t quality;
  uint32_t flags;
  FamoUtf8String label;
} FamoCandidateV2;

// Frozen field span for candidate layout v2. Negotiate this constant instead
// of using a compiler-dependent historical sizeof as a version marker.
#define FAMO_CANDIDATE_V2_STRIDE \
  (offsetof(FamoCandidateV2, label) + sizeof(FamoUtf8String))
#ifdef __cplusplus
static_assert(FAMO_CANDIDATE_V2_STRIDE <= sizeof(FamoCandidateV2),
              "candidate v2 field span exceeds its native structure");
static_assert(FAMO_CANDIDATE_V2_STRIDE % alignof(FamoCandidateV2) == 0,
              "candidate v2 stride must preserve candidate alignment");
#endif

typedef struct FamoCompositionViewV2 {
  uint32_t struct_size;
  uint32_t layout_version;
  uint32_t candidate_layout_version;
  uint32_t candidate_stride;
  FamoUtf8String preedit;
  FamoUtf8String commit;
  const FamoCandidateV2* candidates;
  uint32_t candidate_count;
  uint32_t highlighted_index;
  uint32_t page_index;
  uint32_t page_size;
  // Content bits only. FAMO_COMPOSITION_HANDLED is a v1 compatibility bit;
  // FamoEngineActionResultV2::handled is the sole v2 handled truth.
  uint32_t state_flags;
  uint32_t preedit_sel_start;
  uint32_t preedit_sel_end;
  uint32_t preedit_cursor_pos;
  FamoUtf8String commit_preview;
  FamoUtf8String schema_id;
  FamoUtf8String schema_name;
  uint32_t status_flags;
  uint32_t is_last_page;
} FamoCompositionViewV2;

#define FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE \
  (offsetof(FamoCompositionViewV2, is_last_page) + sizeof(uint32_t))
#ifdef __cplusplus
static_assert(FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE <=
                  sizeof(FamoCompositionViewV2),
              "composition view v2 field span exceeds its native structure");
#endif

// Callers zero-initialize the request and set every version/stride field.
// PROCESS_KEY initializes key.size. Candidate actions use index; PEEK uses
// index+count (count <= FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES); CHANGE_PAGE uses
// value=0 for forward and value=1 for backward. RECOVER uses value for the
// original business action and never mutates. Unused fields remain zero. The
// engine validates struct_size before reading any later field.
typedef struct FamoEngineActionRequestV2 {
  uint32_t struct_size;
  uint32_t action;
  uint32_t view_layout_version;
  uint32_t candidate_layout_version;
  uint32_t candidate_stride;
  uint32_t index;
  uint32_t count;
  int32_t value;
  FamoKeyEvent key;
} FamoEngineActionRequestV2;

#define FAMO_ENGINE_ACTION_REQUEST_V2_REQUIRED_SIZE \
  (offsetof(FamoEngineActionRequestV2, key) + FAMO_KEY_EVENT_REQUIRED_SIZE)
#ifdef __cplusplus
static_assert(FAMO_ENGINE_ACTION_REQUEST_V2_REQUIRED_SIZE <=
                  sizeof(FamoEngineActionRequestV2),
              "action request v2 field span exceeds its native structure");
#endif

// The result and everything reachable from view are one engine-owned
// allocation. Callers never supply result storage and release it exactly once
// through free_result, which does not trust mutable count/stride fields.
#define FAMO_ENGINE_RESULT_RESYNC_REQUIRED (1u << 0)
typedef struct FamoEngineActionResultV2 {
  uint32_t struct_size;
  uint32_t action;
  uint32_t handled;
  // Zero identifies the final snapshot. RESYNC_REQUIRED is a preallocated
  // control receipt: action and handled are already final, while view is empty.
  // Hosts preserve their old UI, do not format/respond, and issue RECOVER.
  uint32_t result_flags;
  FamoCompositionViewV2 view;
} FamoEngineActionResultV2;

#define FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE                  \
  (offsetof(FamoEngineActionResultV2, view) +                       \
   FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE)
#ifdef __cplusplus
static_assert(FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE <=
                  sizeof(FamoEngineActionResultV2),
              "action result v2 field span exceeds its native structure");
#endif

// Engine notifications stay behind the opaque-context boundary. The callback
// borrows every string only for its duration; context is null for engine-wide
// events. For an option event, state_label is the schema-localized label
// resolved by the engine for the exact source context (or an empty string when
// unavailable). The host callback must not call back into the engine.
// A non-null context is publishable only after its create_context operation is
// committed to return FAMO_ENGINE_OK. Engines buffer synchronous create-time
// events until that point and discard them if creation fails; a host must never
// observe a pointer owned by a failed create_context call.
typedef void(FAMO_ENGINE_CALL *FamoEngineNotificationHandlerV2)(
    void* user_data,
    FamoEngineContext* context,
    const FamoUtf8String* message_type,
    const FamoUtf8String* message_value,
    const FamoUtf8String* state_label);

typedef struct FamoEngineApiV2 {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t (FAMO_ENGINE_CALL *get_info)(FamoEngineInfo* out_info);
  int32_t (FAMO_ENGINE_CALL *initialize)(const FamoEngineHostApi* host,
                                         const FamoUtf8String* data_root);
  int32_t (FAMO_ENGINE_CALL *shutdown)(void);
  int32_t (FAMO_ENGINE_CALL *create_context)(
      const FamoUtf8String* schema_id,
      FamoEngineContext** out_context);
  int32_t (FAMO_ENGINE_CALL *destroy_context)(FamoEngineContext* context);
  int32_t (FAMO_ENGINE_CALL *execute_action)(
      FamoEngineContext* context,
      const FamoEngineActionRequestV2* request,
      FamoEngineActionResultV2** out_result);
  int32_t (FAMO_ENGINE_CALL *set_option)(FamoEngineContext* context,
                                         const FamoUtf8String* name,
                                         int32_t value);
  int32_t (FAMO_ENGINE_CALL *get_option)(FamoEngineContext* context,
                                         const FamoUtf8String* name,
                                         int32_t* out_value);
  // Deployment diagnostics go through host.log. v2 deliberately has no
  // string output whose ownership would require another release interface.
  int32_t (FAMO_ENGINE_CALL *deploy_schema)(
      const FamoUtf8String* schema_id);
  int32_t (FAMO_ENGINE_CALL *free_result)(
      FamoEngineActionResultV2* result);
  // Opaque context metadata used by schemas/Lua (for example client_app).
  int32_t (FAMO_ENGINE_CALL *set_property)(
      FamoEngineContext* context,
      const FamoUtf8String* name,
      const FamoUtf8String* value);
  // May be called before initialize so initialization/deployment events are
  // observable. Passing a null handler disables notifications.
  int32_t (FAMO_ENGINE_CALL *set_notification_handler)(
      FamoEngineNotificationHandlerV2 handler,
      void* user_data);
} FamoEngineApiV2;

#define FAMO_ENGINE_API_V2_REQUIRED_SIZE                            \
  (offsetof(FamoEngineApiV2, set_notification_handler) +            \
   sizeof(((FamoEngineApiV2*)0)->set_notification_handler))

FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL FamoCreateEngineApi(
    uint32_t requested_abi_version,
    FamoEngineApi* out_api);

FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL FamoCreateEngineApiV2(
    uint32_t requested_abi_version,
    FamoEngineApiV2* out_api);

// Lifecycle contract for both ABI tables: a host serializes callbacks for one
// loaded engine instance (including callbacks on different contexts). Before
// shutdown it destroys every context and releases every result/view. No table
// callback is legal after shutdown, and no callback may overlap initialize or
// shutdown. Engine exports/table callbacks have the same no-unwind requirement:
// no C++ exception or SEH unwind crosses this C ABI boundary.
//
// v2 notifications are the sole asynchronous exception: they may overlap a
// table call. The engine synchronizes context lookup/destruction and keeps a
// non-null callback context alive for the whole callback. The host callback
// must not re-enter the engine. Disabling the handler waits for callbacks
// already in flight; shutdown disables notifications, joins engine workers,
// and guarantees that no notification runs after shutdown returns.

#ifdef __cplusplus
}
#endif
