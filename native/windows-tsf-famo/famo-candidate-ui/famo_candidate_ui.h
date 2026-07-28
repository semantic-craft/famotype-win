#pragma once
//
// FamoCandidateUI — clean-room candidate-window layout/paint component (M2).
//
// Clean-room boundary == GPL boundary: this component includes ONLY the neutral
// engine ABI + system SDK headers. It names no upstream IME type and copies no
// upstream UI source; the geometry below is authored fresh from the behavioral
// spec (research/candidate-ui-current-state.md §1.2/§1.3), not from any
// *Layout.cpp. Field names that echo intent (e.g. round_corner) are fine.
//
// Split is deliberate: Layout is a *pure function* of (view, skin, input) → rects
// (no HDC, no device — headless, unit-testable HERE, like roundtrip_selfcheck).
// Paint (B5) needs a device and is visual-deferred.

#include <stdint.h>

#include "../engine-api/famo_engine_api.h"  // FamoCompositionView (neutral input)

#ifdef __cplusplus
#define FAMO_CANDIDATE_UI_NOEXCEPT noexcept
extern "C" {
#else
#define FAMO_CANDIDATE_UI_NOEXCEPT
#endif

#define FAMO_CANDIDATE_UI_VERSION 1u

typedef enum FamoCandidateUiResult {
  FAMO_UI_OK = 0,
  FAMO_UI_E_INVALID_ARGUMENT = 0x00010001,
  FAMO_UI_E_NOT_IMPLEMENTED = 0x00010002,
  FAMO_UI_E_PAINT_FAILED = 0x00010003,  // paint caught a fault → host hides popup
  FAMO_UI_E_LAYOUT_FAILED = 0x00010004  // layout caught callback/allocation failure
} FamoCandidateUiResult;

// The three layout families (research §1.3), selected by FamoSkin.layout_type.
// FullScreen is an orthogonal wrapper today; not modelled in B1-B4 (non-goal).
typedef enum FamoLayoutType {
  FAMO_LAYOUT_VERTICAL = 0,      // candidates stacked top→bottom
  FAMO_LAYOUT_HORIZONTAL = 1,    // candidates laid left→right
  FAMO_LAYOUT_VERTICAL_TEXT = 2, // vertical-text: columns right→left, glyphs T→B
  FAMO_LAYOUT_AUTO = 3           // host resolves from the live Rime _vertical option
} FamoLayoutType;

// Integer pixel rect (device pixels after DPI scaling). left/top inclusive,
// right/bottom exclusive — width = right-left, height = bottom-top.
typedef struct FamoRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
} FamoRect;

typedef struct FamoSize {
  int32_t cx;
  int32_t cy;
} FamoSize;

// ─── FamoSkin ────────────────────────────────────────────────────────────────
// The engine ABI carries NO style (correct — research §3.3.4). FamoCandidateUI
// owns its own neutral skin: colors (0xAARRGGBB), the three font specs, the
// layout family, and the style knob-set re-declared as OUR struct. Same origin
// as today's style channel (active Rime color scheme + famo-style.yaml); the yaml
// loader is wired in Phase C — here FamoSkinDefault() supplies a compiled-in set.

#define FAMO_FONT_FACE_MAX 128

typedef struct FamoFontSpec {
  char face[FAMO_FONT_FACE_MAX];  // face string; may be a "face1, face2" fallback list
  float point_size;               // logical point size (scaled by dpi at paint time)
} FamoFontSpec;

typedef struct FamoSkin {
  uint32_t size;  // sizeof(FamoSkin) — size-negotiated like the engine ABI

  FamoLayoutType layout_type;

  // Colors (0xAARRGGBB). A zero alpha means "transparent → do not draw" for the
  // optional affordances (comment / prevpage / nextpage), matching current
  // behavior where a transparent color suppresses the element (research §1.2).
  uint32_t text_color;             // preedit + default candidate text
  uint32_t back_color;             // panel background
  uint32_t border_color;           // panel border
  uint32_t hilited_text_color;     // highlighted candidate text
  uint32_t hilited_back_color;     // highlighted candidate background
  uint32_t candidate_text_color;   // non-highlighted candidate text
  uint32_t label_color;            // candidate label ("1." "a.")
  uint32_t comment_color;          // candidate comment/annotation
  uint32_t hilited_comment_color;  // comment on the highlighted candidate
  uint32_t prevpage_color;         // "<" affordance
  uint32_t nextpage_color;         // ">" affordance
  uint32_t shadow_color;           // drop-shadow color

  // Three DirectWrite formats (research §1.4): label / text / comment.
  FamoFontSpec label_font;
  FamoFontSpec text_font;
  FamoFontSpec comment_font;

  // Metrics (the style knob-set, research §1.3), in logical px @96dpi.
  int32_t margin_x;           // panel inner left/right margin
  int32_t margin_y;           // panel inner top/bottom margin
  int32_t spacing;            // gap between preedit/aux band and the candidate list
  int32_t candidate_spacing;  // gap between adjacent candidates
  int32_t hilite_padding_x;   // highlight rect inflation, x
  int32_t hilite_padding_y;   // highlight rect inflation, y
  int32_t label_spacing;      // gap between label and candidate text
  int32_t comment_spacing;    // gap between candidate text and comment
  int32_t round_corner;       // panel/highlight corner radius
  int32_t border;             // panel border width
  int32_t shadow_radius;      // gaussian shadow blur radius
  int32_t shadow_offset_x;    // shadow offset
  int32_t shadow_offset_y;
  int32_t min_width;
  int32_t min_height;
  int32_t max_width;   // 0 = unlimited
  int32_t max_height;  // 0 = unlimited
  int32_t status_icon_size;  // square status-icon edge (0 = no icon slot)

  // Preedit caret width in logical px @96dpi (0 → built-in default 2).
  // The HOST populates this from SystemParametersInfo(SPI_GETCARETWIDTH) — the
  // user-configurable Windows setting (1..20, registry CaretWidth). It arrives
  // through the skin rather than being queried here on purpose: this component
  // is clean-room + headless-testable, so a system-dependent value read inside
  // paint would make bitmap_smoke's pixel assertions depend on the test
  // machine's accessibility settings.
  int32_t caret_width;

  // Secondary surface token (`card2`). Appended for size-negotiated compatibility;
  // status-bar callers built against an older struct fall back to deriving it.
  uint32_t card2_color;

  // Candidate-panel additions. Appended so older callers keep their existing
  // layout; absent fields default to show-preedit on and preview off.
  uint32_t show_preedit;
  uint32_t preview_pages;
  uint32_t preview_rows;  // clamped to 1..2
} FamoSkin;

// Compiled-in neutral default skin (Vertical, opaque dark-on-light).
FamoSkin FamoSkinDefault(void) FAMO_CANDIDATE_UI_NOEXCEPT;

// ─── Text measurement seam ───────────────────────────────────────────────────
// Layout is headless (no DirectWrite device), yet needs text extents. The host
// supplies a measurement callback; B5's real host passes a DirectWrite-backed
// one, tests pass a deterministic monospace estimator. If measure == NULL the
// layout falls back to a built-in monospace estimate (cell = point_size*ratio),
// so geometry stays deterministic + testable without a device.
//
//   which: 0=label font, 1=text font, 2=comment font.
//   utf8/utf8_len: the string to measure (UTF-8 bytes).
//   Returns the advance width in device px; caller derives height from the font.
typedef int32_t (*FamoMeasureTextFn)(void* user, int32_t which,
                                     const char* utf8, uint32_t utf8_len);

typedef struct FamoLayoutInput {
  uint32_t size;

  // Caret rect (device px, screen coords) the composition is anchored to — the
  // caret's bottom-left drives the popup anchor (research §1.3).
  FamoRect caret_rect;

  // Monitor work area (device px, screen coords) for screen-edge flip clamping.
  FamoRect work_area;

  uint32_t dpi;  // effective per-monitor DPI; skin metrics scale by dpi/96.

  // Server-provided auxiliary/tips string (schema-switch toast, deploy/error
  // tips). NOT an engine ABI field (research §3.3.5 resolved): it is server-
  // generated UX, so it flows server→component here, not through
  // FamoCompositionView. Empty (data==NULL) → no aux band.
  FamoUtf8String aux;

  // Text measurement seam (see above). NULL → deterministic monospace fallback.
  FamoMeasureTextFn measure;
  void* measure_user;

  // Read-only candidates from the following one or two pages. They are laid
  // out only under a horizontal compact row; labels/comments are ignored.
  const FamoCandidate* preview_candidates;
  uint32_t preview_candidate_count;
  uint32_t preview_page_size;
} FamoLayoutInput;

// Per-candidate rect bundle. All rects are in the popup's content coordinate
// space (origin at content top-left, pre-anchor); the host translates by the
// final origin from FamoLayoutResult.
typedef struct FamoCandidateRects {
  FamoRect bounds;     // full candidate row/cell (hit-test + hover target)
  FamoRect label;      // label sub-rect ("1.")  (empty if no label)
  FamoRect text;       // candidate text sub-rect
  FamoRect comment;    // comment sub-rect (empty if no/transparent comment)
  uint32_t has_comment;
} FamoCandidateRects;

#define FAMO_MAX_LAID_CANDIDATES 32u
#define FAMO_MAX_PREVIEW_CANDIDATES 64u

typedef struct FamoLayoutResult {
  uint32_t size;

  FamoSize content_size;  // popup content size in device px (before shadow)

  // Symmetric drop-shadow margin (device px) around content; 0 when the shadow is
  // disabled (transparent shadow_color or shadow_radius<=0). The paint buffer is
  // (content + 2*shadow_margin): Paint draws content at (shadow_margin,
  // shadow_margin) and the gaussian shadow in the surrounding ring; the host sizes
  // the layered bitmap to that and positions the window at (origin - shadow_margin)
  // so the content still lands at the caret-anchored origin.
  int32_t shadow_margin;

  FamoRect preedit;   // preedit/composition band (empty if none)
  FamoRect aux;       // aux/tips band (empty if none)
  FamoRect highlight; // highlighted-candidate background rect (empty if none)
  FamoRect status_icon;   // status-icon slot (empty if none)
  FamoRect prev_page;     // "<" hit-rect (empty if page_index==0)
  FamoRect next_page;     // ">" hit-rect (empty if is_last_page)

  uint32_t candidate_count;  // #laid candidates (<= FAMO_MAX_LAID_CANDIDATES)
  FamoCandidateRects candidates[FAMO_MAX_LAID_CANDIDATES];

  uint32_t preview_candidate_count;
  FamoCandidateRects preview_candidates[FAMO_MAX_PREVIEW_CANDIDATES];

  // Preedit selection/caret converted to UTF-16 wchar offsets (research §3.3.3:
  // ABI carries UTF-8 byte offsets; the current UI indexes preedit by wchar).
  uint32_t preedit_sel_start_wchar;
  uint32_t preedit_sel_end_wchar;
  uint32_t preedit_cursor_wchar;

  // Final anchored origin (screen coords, device px) after caret-follow + flip,
  // and whether the popup was flipped above the caret (drives vertical order
  // reversal, research §1.3). content_size is unchanged by the flip.
  int32_t origin_x;
  int32_t origin_y;
  uint32_t flipped;
} FamoLayoutResult;

// FamoCandidateUiLayout has no caller-capacity parameter: unlike the input
// structs, this output is not size-negotiated and must not grow in-place.
// Keep a fixed span so the ABI canary detects any future wider write.
#define FAMO_LAYOUT_RESULT_STABLE_SIZE \
  (offsetof(FamoLayoutResult, flipped) + sizeof(uint32_t))

// ─── DirectWrite/D2D text resources (B5) ─────────────────────────────────────
// Opaque rendering resources shared by Paint and the measurement callback. The
// host creates one instance, then reconfigures its DPI-dependent text formats
// and single static shadow when the Skin or DPI changes. Device resources that
// are stable across BindDC calls stay warm. Returns NULL on creation failure.
typedef struct FamoTextResources FamoTextResources;

FamoTextResources* FamoTextResourcesCreate(
    const FamoSkin* skin, uint32_t dpi) FAMO_CANDIDATE_UI_NOEXCEPT;
int32_t FamoTextResourcesReconfigure(FamoTextResources* res,
                                     const FamoSkin* skin,
                                     uint32_t dpi) FAMO_CANDIDATE_UI_NOEXCEPT;
// Drop only target-bound D2D resources after a device-loss style paint failure.
// Stable factories, formats, the bounded surface and static shadow stay warm.
void FamoTextResourcesDiscardDeviceResources(
    FamoTextResources* res) FAMO_CANDIDATE_UI_NOEXCEPT;
void FamoTextResourcesDestroy(
    FamoTextResources* res) FAMO_CANDIDATE_UI_NOEXCEPT;

// FamoMeasureTextFn-compatible measurement over the cached formats. Wire this as
// FamoLayoutInput.measure with a live FamoTextResources* as measure_user.
// which: 0=label 1=text 2=comment. Returns advance width in device px (0 on any
// error / empty), so a failed measure degrades to a zero-width, never a crash.
int32_t FamoTextMeasure(void* user, int32_t which, const char* utf8,
                        uint32_t utf8_len) FAMO_CANDIDATE_UI_NOEXCEPT;

// ─── Public entry points ─────────────────────────────────────────────────────

// Compute the full layout (pure math, headless, testable HERE). Fills *out.
int32_t FamoCandidateUiLayout(const FamoCompositionView* view,
                              const FamoSkin* skin,
                              const FamoLayoutInput* input,
                              FamoLayoutResult* out)
    FAMO_CANDIDATE_UI_NOEXCEPT;

// Paint a computed layout onto a 32-bit top-down premultiplied-alpha memory DC:
// GDI+ shapes (background / highlight / round-rect / border) + D2D/DirectWrite
// text (preedit / aux / per-candidate label/text/comment / page glyphs), the
// text composited over the shapes (B5, design §6). The DC's bitmap MUST be sized
// (content_size + 2*shadow_margin): content is drawn at (shadow_margin,
// shadow_margin) and the gaussian drop shadow fills the ring around it. Takes the
// `view` (candidate + preedit strings — FamoLayoutResult carries only rects) and
// `input` (aux string + dpi), plus a FamoTextResources built for this (skin,dpi)
// so painted advances match the ones layout measured. mem_dc is void* so this header stays free of
// <windows.h> for the headless core. Alpha==0 (transparent) optional elements are
// skipped, matching layout + current behavior. Returns FAMO_UI_OK, or
// FAMO_UI_E_PAINT_FAILED on a caught fault — the composition is never touched
// (crash isolation, design §6). UpdateLayeredWindow compositing is host-side.
int32_t FamoCandidateUiPaint(const FamoCompositionView* view,
                             const FamoSkin* skin,
                             const FamoLayoutInput* input,
                             const FamoLayoutResult* layout,
                             FamoTextResources* res,
                             void* mem_dc) FAMO_CANDIDATE_UI_NOEXCEPT;

// ─── Floating status bar ─────────────────────────────────────────────────────
// The always-visible toggle bar is a second consumer of the skin and the text
// resources, not of the candidate layout: it has no FamoCompositionView and no
// FamoLayoutResult. Its host owns dragging, DPI and hit-testing, so it lays the
// buttons out itself and passes the finished rects here; this entry point only
// draws them.

typedef struct FamoStatusBarButton {
  FamoRect bounds;     // bar-local device px
  const char* label;   // NUL-terminated UTF-8; NULL/empty draws no glyph
  uint32_t on;         // option currently enabled; label carries the idle state
  uint32_t hover;
  uint32_t pressed;
} FamoStatusBarButton;

typedef struct FamoStatusBarSpec {
  uint32_t size;      // sizeof(FamoStatusBarSpec)
  FamoSize bar_size;  // device px; must match the DC's bitmap
  uint32_t dpi;       // effective per-monitor DPI; skin metrics scale by dpi/96
  uint32_t button_count;
  const FamoStatusBarButton* buttons;
} FamoStatusBarSpec;

// Paint the bar onto a 32-bit top-down premultiplied-alpha memory DC sized
// bar_size, as the design system's segmented control: a round-rect trough with
// its hairline border, the buttons tiled across it as segments sharing edges
// (only the outermost corners rounded), a transient accent fill while pressed,
// and a hairline divider between two idle neighbours. `buttons` must be in
// left-to-right order and inset from the trough edge — the first segment's left
// inset is what the segment corner radius is derived from.
//
// `res` must be a FamoTextResources built for this dpi; its text font (index 1)
// supplies the labels, so a host wanting a smaller bar font builds those
// resources from a skin copy with a smaller text_font.point_size.
//
// Labels go through DirectWrite for the same reason the candidate text does:
// GDI text leaves the alpha channel at zero, which UpdateLayeredWindow presents
// as fully transparent. They are centred on their inked extents rather than
// their layout box, so CJK punctuation (which sits in one corner of a
// full-width em box) lines up with the square glyphs beside it.
//
// Returns FAMO_UI_OK, or FAMO_UI_E_PAINT_FAILED on a caught fault — a failed
// bar paint must never take the input method with it.
int32_t FamoStatusBarPaint(const FamoStatusBarSpec* spec, const FamoSkin* skin,
                           FamoTextResources* res,
                           void* mem_dc) FAMO_CANDIDATE_UI_NOEXCEPT;

// ─── Standalone helpers (exposed so tests can exercise the correctness trap) ──

// Convert a UTF-8 byte offset into a UTF-16 wchar (UTF-16 code-unit) offset over
// [utf8, utf8+utf8_len). Astral code points count as 2 (surrogate pair), matching
// wchar_t on Windows. Clamps a byte offset landing mid-sequence to the code-point
// boundary at/after it. Out-of-range byte_off clamps to the full wchar length.
uint32_t FamoUtf8ByteToWchar(const char* utf8, uint32_t utf8_len,
                             uint32_t byte_off)
    FAMO_CANDIDATE_UI_NOEXCEPT;

// The screen-edge flip decision (research §1.3): given caret + content size +
// work area, produce the anchored origin and the flipped flag. Pure function.
void FamoComputeAnchor(const FamoRect* caret, const FamoRect* work_area,
                       FamoSize content, int32_t* out_x, int32_t* out_y,
                       uint32_t* out_flipped) FAMO_CANDIDATE_UI_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef FAMO_CANDIDATE_UI_NOEXCEPT
