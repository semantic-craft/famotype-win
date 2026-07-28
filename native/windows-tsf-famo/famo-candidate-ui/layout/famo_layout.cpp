// FamoCandidateUI layout engine (B3) — PURE MATH, no HDC / no device.
//
// Authored fresh from the behavioral spec (research §1.2 = the rect set to
// produce; §1.3 = the layout families + caret-follow + screen-edge flip). The
// spacing/baseline model is ours: a candidate row is
//   [label][label_spacing][text][comment_spacing][comment]
// inflated by hilite_padding for the highlight; rows stack per family; a preedit
// / aux band sits above the list; prev/next-page affordances are appended per the
// page state. No *Layout.cpp was consulted.
//
// Headless text extents: the host supplies FamoLayoutInput.measure (a DirectWrite
// device in B5); tests pass a deterministic estimator. measure==NULL → built-in
// monospace fallback, so geometry is deterministic + unit-testable here.

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "../famo_candidate_access.h"
#include "../famo_candidate_ui.h"

namespace {

inline int32_t Scale(int32_t v, uint32_t dpi) {
  // dpi/96 rounding to nearest; dpi==0 treated as 96 (no scaling).
  if (dpi == 0) dpi = 96;
  return static_cast<int32_t>((static_cast<int64_t>(v) * dpi + 48) / 96);
}

inline int32_t RectW(const FamoRect& r) { return r.right - r.left; }
inline int32_t RectH(const FamoRect& r) { return r.bottom - r.top; }
inline FamoRect EmptyRect() { return FamoRect{0, 0, 0, 0}; }

// Decode one UTF-8 sequence starting at s[i]; returns its byte length (1..4),
// clamped so it never reads past len. Malformed lead bytes advance by 1.
uint32_t Utf8SeqLen(const char* s, uint32_t len, uint32_t i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  uint32_t n = 1;
  if (c < 0x80u) n = 1;
  else if ((c >> 5) == 0x6u) n = 2;   // 110xxxxx
  else if ((c >> 4) == 0xEu) n = 3;   // 1110xxxx
  else if ((c >> 3) == 0x1Eu) n = 4;  // 11110xxx
  else n = 1;                          // stray continuation / invalid lead
  if (i + n > len) n = len - i;        // clamp to buffer
  return n == 0 ? 1u : n;
}

// UTF-16 code units for one decoded scalar of byte-length seq_len: astral
// (4-byte UTF-8, U+10000+) → 2 (surrogate pair); else 1.
inline uint32_t Utf16UnitsForSeq(uint32_t seq_len) { return seq_len >= 4 ? 2u : 1u; }

// Deterministic monospace width fallback: cell width ≈ point_size * 0.6 per
// UTF-16 code unit (astral counts as 2 cells — matches a typical wide glyph pair
// well enough for a headless estimate). Scaled by DPI.
int32_t FallbackMeasure(const FamoFontSpec& font, const char* utf8, uint32_t len,
                        uint32_t dpi) {
  if (!utf8 || len == 0) return 0;
  uint32_t units = 0;
  for (uint32_t i = 0; i < len;) {
    uint32_t n = Utf8SeqLen(utf8, len, i);
    units += Utf16UnitsForSeq(n);
    i += n;
  }
  int32_t cell = static_cast<int32_t>(font.point_size * 0.6f + 0.5f);
  if (cell < 1) cell = 1;
  return Scale(cell * static_cast<int32_t>(units), dpi);
}

// Font height estimate (used for row/line heights). ≈ point_size * 1.35, scaled.
int32_t FontHeight(const FamoFontSpec& font, uint32_t dpi) {
  int32_t h = static_cast<int32_t>(font.point_size * 1.35f + 0.5f);
  if (h < 1) h = 1;
  return Scale(h, dpi);
}

int32_t MeasureText(const FamoLayoutInput& in, int32_t which,
                    const FamoFontSpec& font, const FamoUtf8String& s) {
  if (!s.data || s.length_bytes == 0) return 0;
  if (in.measure) return in.measure(in.measure_user, which, s.data, s.length_bytes);
  return FallbackMeasure(font, s.data, s.length_bytes, in.dpi);
}

}  // namespace

// ── Correctness trap (research §3.3.3): UTF-8 byte offset → UTF-16 wchar ──────
extern "C" uint32_t FamoUtf8ByteToWchar(const char* utf8, uint32_t utf8_len,
                                        uint32_t byte_off) {
  if (!utf8 || utf8_len == 0) return 0;
  if (byte_off > utf8_len) byte_off = utf8_len;
  uint32_t wchars = 0;
  uint32_t i = 0;
  while (i < byte_off) {
    uint32_t n = Utf8SeqLen(utf8, utf8_len, i);
    // A byte_off landing mid-sequence rounds up to include the whole scalar
    // (offsets should be code-point aligned; this is defensive).
    wchars += Utf16UnitsForSeq(n);
    i += n;
  }
  return wchars;
}

// ── Screen-edge flip decision (research §1.3): caret-follow + flip-above ──────
extern "C" void FamoComputeAnchor(const FamoRect* caret, const FamoRect* work,
                                  FamoSize content, int32_t* out_x,
                                  int32_t* out_y, uint32_t* out_flipped) {
  const int32_t kGap = 6;  // 6px below the caret (research §1.3)
  int32_t x = caret->left;
  int32_t y = caret->bottom + kGap;
  uint32_t flipped = 0;

  // Flip above if the below-caret placement would exceed the work-area bottom.
  if (y + content.cy > work->bottom) {
    int32_t up = caret->top - content.cy - kGap;
    // Only accept the flip if it fits better (>= work top); else keep below and
    // clamp — never render off the top edge.
    if (up >= work->top) {
      y = up;
      flipped = 1;
    }
  }

  // Clamp x into [work.left, work.right - width].
  int32_t max_x = work->right - content.cx;
  if (x > max_x) x = max_x;
  if (x < work->left) x = work->left;
  // Clamp y into [work.top, work.bottom - height] (bottom clamp for the non-flip
  // fallback path above).
  int32_t max_y = work->bottom - content.cy;
  if (y > max_y) y = max_y;
  if (y < work->top) y = work->top;

  *out_x = x;
  *out_y = y;
  *out_flipped = flipped;
}

// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct Metrics {
  int32_t margin_x, margin_y, spacing, cand_spacing;
  int32_t hpad_x, hpad_y, label_spacing, comment_spacing;
  int32_t min_w, min_h, max_w, max_h, status_icon;
  int32_t label_h, text_h, comment_h;
  int32_t page_glyph_w;  // width reserved for a chevron affordance
};

Metrics ScaleMetrics(const FamoSkin& sk, const FamoLayoutInput& in) {
  Metrics m;
  const uint32_t dpi = in.dpi;
  m.margin_x        = Scale(sk.margin_x, dpi);
  m.margin_y        = Scale(sk.margin_y, dpi);
  m.spacing         = Scale(sk.spacing, dpi);
  m.cand_spacing    = Scale(sk.candidate_spacing, dpi);
  m.hpad_x          = Scale(sk.hilite_padding_x, dpi);
  m.hpad_y          = Scale(sk.hilite_padding_y, dpi);
  m.label_spacing   = Scale(sk.label_spacing, dpi);
  m.comment_spacing = Scale(sk.comment_spacing, dpi);
  m.min_w           = Scale(sk.min_width, dpi);
  m.min_h           = Scale(sk.min_height, dpi);
  m.max_w           = Scale(sk.max_width, dpi);
  m.max_h           = Scale(sk.max_height, dpi);
  m.status_icon     = Scale(sk.status_icon_size, dpi);
  m.label_h         = FontHeight(sk.label_font, dpi);
  m.text_h          = FontHeight(sk.text_font, dpi);
  m.comment_h       = FontHeight(sk.comment_font, dpi);
  // A page affordance is ~ one text glyph wide.
  m.page_glyph_w    = FallbackMeasure(sk.text_font, "›", 3, dpi);
  if (m.page_glyph_w < 1) m.page_glyph_w = Scale(8, dpi);
  return m;
}

// Whether a candidate has a drawable (non-transparent) comment.
bool CommentVisible(const FamoSkin& sk, const FamoCandidate& c) {
  return c.comment.data && c.comment.length_bytes > 0 &&
         (sk.comment_color >> 24) != 0u;
}

// Whether the "<"/">" affordances are drawable at all (non-transparent color).
bool PrevVisible(const FamoSkin& sk) { return (sk.prevpage_color >> 24) != 0u; }
bool NextVisible(const FamoSkin& sk) { return (sk.nextpage_color >> 24) != 0u; }

bool ShowPreedit(const FamoSkin& sk) {
  return sk.size < offsetof(FamoSkin, preview_pages) || sk.show_preedit != 0;
}

bool PreviewPages(const FamoSkin& sk) {
  return sk.size >= offsetof(FamoSkin, preview_rows) && sk.preview_pages != 0;
}

// Symmetric drop-shadow margin (device px): wide enough to hold the gaussian blur
// spread (~radius each side) plus the shadow offset. 0 when the shadow is off
// (transparent shadow_color or non-positive radius) — then the buffer == content
// and paint draws at the origin (unchanged pre-shadow behavior).
int32_t ShadowMargin(const FamoSkin& sk, uint32_t dpi) {
  if ((sk.shadow_color >> 24) == 0u || sk.shadow_radius <= 0) return 0;
  int32_t r = Scale(sk.shadow_radius, dpi);
  int32_t ax = sk.shadow_offset_x < 0 ? -sk.shadow_offset_x : sk.shadow_offset_x;
  int32_t ay = sk.shadow_offset_y < 0 ? -sk.shadow_offset_y : sk.shadow_offset_y;
  int32_t off = Scale(ax > ay ? ax : ay, dpi);
  return r + (r + 1) / 2 + off + 2;  // blur spread + offset + a little slack
}

// Measure one candidate's label/text/comment advances (device px).
struct CandExtent {
  int32_t label_w, label_h, text_w, comment_w;
  bool has_label, has_comment;
};

CandExtent MeasureCand(const FamoSkin& sk, const FamoLayoutInput& in,
                       const FamoCandidate& c, bool vertical = false) {
  CandExtent e;
  std::memset(&e, 0, sizeof(e));
  if (c.label.data && c.label.length_bytes > 0) {
    if (vertical) {
      std::string label(c.label.data, c.label.length_bytes);
      label.push_back('.');
      const FamoUtf8String dotted{sizeof(FamoUtf8String), label.data(),
                                  static_cast<uint32_t>(label.size())};
      e.label_w = MeasureText(in, 1, sk.text_font, dotted);
      e.label_h = FontHeight(sk.text_font, in.dpi);
    } else {
      e.label_w = MeasureText(in, 0, sk.label_font, c.label);
      e.label_h = FontHeight(sk.label_font, in.dpi);
    }
    e.has_label = true;
  }
  e.text_w = MeasureText(in, 1, sk.text_font, c.text);
  if (CommentVisible(sk, c)) {
    e.comment_w = MeasureText(in, 2, sk.comment_font, c.comment);
    e.has_comment = true;
  }
  return e;
}

// Row content width = label + spacing + text + (spacing + comment).
int32_t CandInlineWidth(const Metrics& m, const CandExtent& e) {
  int32_t w = e.text_w;
  if (e.has_label) w += e.label_w + m.label_spacing;
  if (e.has_comment) w += m.comment_spacing + e.comment_w;
  return w;
}

int32_t CandRowHeight(const Metrics& m, const CandExtent& e) {
  int32_t h = m.text_h;
  if (e.has_label && e.label_h > h) h = e.label_h;
  if (e.has_comment && m.comment_h > h) h = m.comment_h;
  return h;
}

// Place label/text/comment sub-rects inside a row laid out left→right, given the
// row's content-left x and vertical center band [top, top+row_h). Returns the
// x cursor just past the content (for width accounting).
int32_t PlaceRowInline(const Metrics& m, const CandExtent& e, int32_t x,
                       int32_t top, int32_t row_h, FamoCandidateRects* out) {
  auto centered = [&](int32_t x0, int32_t w, int32_t h) -> FamoRect {
    int32_t y0 = top + (row_h - h) / 2;
    return FamoRect{x0, y0, x0 + w, y0 + h};
  };
  if (e.has_label) {
    out->label = centered(x, e.label_w, e.label_h);
    x += e.label_w + m.label_spacing;
  } else {
    out->label = EmptyRect();
  }
  out->text = centered(x, e.text_w, m.text_h);
  x += e.text_w;
  if (e.has_comment) {
    x += m.comment_spacing;
    out->comment = centered(x, e.comment_w, m.comment_h);
    out->has_comment = 1;
    x += e.comment_w;
  } else {
    out->comment = EmptyRect();
    out->has_comment = 0;
  }
  return x;
}

// Clamp final content size into [min, max] (0 max = unlimited).
FamoSize ClampSize(const Metrics& m, int32_t w, int32_t h) {
  if (w < m.min_w) w = m.min_w;
  if (h < m.min_h) h = m.min_h;
  if (m.max_w > 0 && w > m.max_w) w = m.max_w;
  if (m.max_h > 0 && h > m.max_h) h = m.max_h;
  return FamoSize{w, h};
}

// The band above the candidate list: preedit line, then aux line. Returns the
// band height and fills the preedit/aux rects (content coords). max_content_w is
// updated with band widths.
int32_t LayoutBand(const FamoSkin& sk, const FamoLayoutInput& in, const Metrics& m,
                   const FamoCompositionView& view, int32_t x0, int32_t y0,
                   FamoRect* preedit_out, FamoRect* aux_out,
                   int32_t* max_content_w) {
  int32_t y = y0;
  *preedit_out = EmptyRect();
  *aux_out = EmptyRect();

  bool has_preedit = ShowPreedit(sk) && view.preedit.data &&
                     view.preedit.length_bytes > 0;
  if (has_preedit) {
    int32_t w = MeasureText(in, 1, sk.text_font, view.preedit);
    *preedit_out = FamoRect{x0, y, x0 + w, y + m.text_h};
    if (w > *max_content_w) *max_content_w = w;
    y += m.text_h;
  }
  bool has_aux = in.aux.data && in.aux.length_bytes > 0;
  if (has_aux) {
    if (has_preedit) y += m.cand_spacing;
    int32_t w = MeasureText(in, 2, sk.comment_font, in.aux);
    *aux_out = FamoRect{x0, y, x0 + w, y + m.comment_h};
    if (w > *max_content_w) *max_content_w = w;
    y += m.comment_h;
  }
  return y - y0;  // band height (0 if neither)
}

}  // namespace

// ── The layout entry point ───────────────────────────────────────────────────
extern "C" int32_t FamoCandidateUiLayout(const FamoCompositionView* view,
                                         const FamoSkin* skin,
                                         const FamoLayoutInput* input,
                                         FamoLayoutResult* out) {
  if (!view || !skin || !input || !out)
    return FAMO_UI_E_INVALID_ARGUMENT;
  if (view->size < offsetof(FamoCompositionView, preedit_sel_start) ||
      skin->size < offsetof(FamoSkin, caret_width) ||
      input->size < offsetof(FamoLayoutInput, preview_candidates))
    return FAMO_UI_E_INVALID_ARGUMENT;
  if (!famo_candidate_ui::ReadableUtf8(view->preedit) ||
      !famo_candidate_ui::ReadableUtf8(input->aux) ||
      (view->candidate_count > 0 && !view->candidates))
    return FAMO_UI_E_INVALID_ARGUMENT;
  const bool has_preview_fields =
      input->size >= offsetof(FamoLayoutInput, preview_page_size) +
                         sizeof(input->preview_page_size);
  if (has_preview_fields && input->preview_candidate_count > 0 &&
      !input->preview_candidates)
    return FAMO_UI_E_INVALID_ARGUMENT;

  std::memset(out, 0, sizeof(*out));
  out->size = static_cast<uint32_t>(sizeof(FamoLayoutResult));

  const FamoSkin& sk = *skin;
  const FamoLayoutInput& in = *input;
  const Metrics m = ScaleMetrics(sk, in);

  const int32_t panel_margin_x =
      sk.layout_type == FAMO_LAYOUT_HORIZONTAL
          ? m.margin_x
          : (std::max)(1, m.margin_x / 2);
  const int32_t x0 = panel_margin_x;
  const int32_t y0 = m.margin_y;

  // Preedit/aux band first (common to all families).
  int32_t max_content_w = 0;
  int32_t band_h = LayoutBand(sk, in, m, *view, x0, y0, &out->preedit, &out->aux,
                              &max_content_w);
  int32_t list_top = y0 + band_h;
  if (band_h > 0) list_top += m.spacing;

  // Preedit sel/cursor → wchar (correctness trap). Guard on view.size spanning
  // the v1.1 offset fields.
  if (view->preedit.data && view->size >= offsetof(FamoCompositionView, commit_preview)) {
    const char* p = view->preedit.data;
    uint32_t plen = view->preedit.length_bytes;
    out->preedit_sel_start_wchar = FamoUtf8ByteToWchar(p, plen, view->preedit_sel_start);
    out->preedit_sel_end_wchar   = FamoUtf8ByteToWchar(p, plen, view->preedit_sel_end);
    out->preedit_cursor_wchar    = FamoUtf8ByteToWchar(p, plen, view->preedit_cursor_pos);
  }

  const uint32_t n = view->candidate_count < FAMO_MAX_LAID_CANDIDATES
                         ? view->candidate_count
                         : FAMO_MAX_LAID_CANDIDATES;
  std::array<FamoCandidate, FAMO_MAX_LAID_CANDIDATES> candidates{};
  for (uint32_t i = 0; i < n; ++i) {
    if (!famo_candidate_ui::ReadCandidate(view->candidates,
                                           view->candidate_count, i,
                                           &candidates[i]) ||
        !famo_candidate_ui::ReadableCandidate(candidates[i]))
      return FAMO_UI_E_INVALID_ARGUMENT;
  }
  out->candidate_count = n;

  // Page affordances: previous if not the first page; next unless the engine
  // says this is the last page (research §1.2/§1.3). Guard
  // is_last_page read on view.size (v1.2 field).
  bool has_is_last =
      view->size >= (offsetof(FamoCompositionView, is_last_page) + sizeof(uint32_t));
  bool is_last = has_is_last ? (view->is_last_page != 0) : (view->candidate_count < view->page_size);
  bool want_prev = PrevVisible(sk) && view->page_index > 0;
  bool want_next = NextVisible(sk) && !is_last;

  const int32_t hl = view->highlighted_index;

  if (sk.layout_type == FAMO_LAYOUT_HORIZONTAL) {
    // Candidates laid left→right on one row; page affordances flank the row.
    int32_t row_h = 0;
    for (uint32_t i = 0; i < n; ++i)
      row_h =
          (std::max)(row_h, CandRowHeight(m, MeasureCand(sk, in, candidates[i])));
    if (row_h == 0) row_h = m.text_h;

    int32_t x = x0;
    if (want_prev) {
      out->prev_page = FamoRect{x, list_top, x + m.page_glyph_w, list_top + row_h};
      x += m.page_glyph_w + m.cand_spacing;
    }
    for (uint32_t i = 0; i < n; ++i) {
      CandExtent e = MeasureCand(sk, in, candidates[i]);
      int32_t inline_w = CandInlineWidth(m, e);
      int32_t bx = x;
      int32_t bw = inline_w + 2 * m.hpad_x;
      int32_t bh = row_h + 2 * m.hpad_y;
      out->candidates[i].bounds = FamoRect{bx, list_top, bx + bw, list_top + bh};
      PlaceRowInline(m, e, bx + m.hpad_x, list_top + m.hpad_y, row_h,
                     &out->candidates[i]);
      x = bx + bw;
      if (i + 1 < n) x += m.cand_spacing;
    }
    if (want_next) {
      x += m.cand_spacing;
      out->next_page = FamoRect{x, list_top, x + m.page_glyph_w, list_top + row_h};
      x += m.page_glyph_w;
    }
    int32_t list_w = x - x0;
    if (list_w > max_content_w) max_content_w = list_w;
    int32_t list_bottom = list_top + row_h + 2 * m.hpad_y;

    const bool preview_available =
        PreviewPages(sk) &&
        in.size >= offsetof(FamoLayoutInput, preview_page_size) +
                       sizeof(in.preview_page_size) &&
        in.preview_candidates && in.preview_candidate_count > 0 &&
        in.preview_page_size > 0;
    if (preview_available) {
      const uint32_t max_preview = (std::min)(
          in.preview_candidate_count,
          static_cast<uint32_t>(FAMO_MAX_PREVIEW_CANDIDATES));
      const uint32_t wanted_rows =
          sk.size >= sizeof(FamoSkin) ? (std::min)(2u, (std::max)(1u, sk.preview_rows))
                                      : 2u;
      const uint32_t count = static_cast<uint32_t>((std::min)(
          static_cast<uint64_t>(max_preview),
          static_cast<uint64_t>(in.preview_page_size) * wanted_rows));
      std::array<FamoCandidate, FAMO_MAX_PREVIEW_CANDIDATES>
          preview_candidates{};
      for (uint32_t i = 0; i < count; ++i) {
        if (!famo_candidate_ui::ReadCandidate(
                in.preview_candidates, in.preview_candidate_count, i,
                &preview_candidates[i]) ||
            !famo_candidate_ui::ReadableCandidate(preview_candidates[i]))
          return FAMO_UI_E_INVALID_ARGUMENT;
      }
      int32_t preview_y = list_bottom + m.cand_spacing;
      for (uint32_t base = 0; base < count; base += in.preview_page_size) {
        int32_t preview_x = x0;
        const uint32_t row_end =
            (std::min)(count, base + in.preview_page_size);
        for (uint32_t i = base; i < row_end; ++i) {
          const FamoCandidate& candidate = preview_candidates[i];
          const int32_t text_w =
              MeasureText(in, 1, sk.text_font, candidate.text);
          FamoCandidateRects& rect = out->preview_candidates[i];
          rect.bounds = {preview_x, preview_y,
                         preview_x + text_w + 2 * m.hpad_x,
                         preview_y + m.text_h + 2 * m.hpad_y};
          rect.text = {preview_x + m.hpad_x, preview_y + m.hpad_y,
                       preview_x + m.hpad_x + text_w,
                       preview_y + m.hpad_y + m.text_h};
          preview_x = rect.bounds.right + m.cand_spacing;
        }
        const int32_t preview_w = preview_x - x0 - m.cand_spacing;
        if (preview_w > max_content_w)
          max_content_w = preview_w;
        preview_y += m.text_h + 2 * m.hpad_y + m.cand_spacing;
      }
      out->preview_candidate_count = count;
      list_bottom = preview_y - m.cand_spacing;
    }

    int32_t content_w = max_content_w + 2 * panel_margin_x;
    int32_t content_h = list_bottom + m.margin_y;
    out->content_size = ClampSize(m, content_w, content_h);
    // The selection pill is exactly its row. It used to bleed 4px up and 9px
    // down into the panel's outer padding (macOS parity), which clamped it flush
    // to the bottom edge and left only 2px between it and the preedit's dotted
    // rule — the panel read as two stacked blocks, not a card with a selection.
    if (hl >= 0 && static_cast<uint32_t>(hl) < n)
      out->highlight = out->candidates[hl].bounds;
  } else {
    // Vertical + VerticalText: candidates stack top→bottom. (VerticalText differs
    // in glyph orientation at PAINT time, B5; its box geometry stacks like
    // Vertical, so the rect set is shared here — the render pass rotates glyphs.)
    int32_t y = list_top;
    int32_t list_w = 0;
    const int32_t row_spacing = 0;  // macOS vertical full-width rows abut
    const int32_t row_pad_x = m.hpad_x + Scale(2, in.dpi);
    for (uint32_t i = 0; i < n; ++i) {
      CandExtent e = MeasureCand(sk, in, candidates[i], true);
      int32_t row_h = CandRowHeight(m, e);
      int32_t inline_w = CandInlineWidth(m, e);
      int32_t bw = inline_w + 2 * row_pad_x;
      int32_t bh = row_h + 2 * m.hpad_y;
      out->candidates[i].bounds = FamoRect{x0, y, x0 + bw, y + bh};
      PlaceRowInline(m, e, x0 + row_pad_x, y + m.hpad_y, row_h,
                     &out->candidates[i]);
      if (static_cast<int32_t>(i) == hl) out->highlight = out->candidates[i].bounds;
      if (bw > list_w) list_w = bw;
      y += bh;
      if (i + 1 < n) y += row_spacing;
    }
    if (list_w > max_content_w) max_content_w = list_w;
    const int32_t full_row_width = max_content_w;
    for (uint32_t i = 0; i < n; ++i) {
      out->candidates[i].bounds.right = x0 + full_row_width;
      if (static_cast<int32_t>(i) == hl)
        out->highlight = out->candidates[i].bounds;
    }

    // Page affordances stack below the list on their own row (prev then next).
    int32_t page_row_top = y;
    if (want_prev || want_next) {
      if (n > 0) page_row_top += m.cand_spacing;
      int32_t px = x0;
      int32_t ph = m.text_h;
      if (want_prev) {
        out->prev_page = FamoRect{px, page_row_top, px + m.page_glyph_w, page_row_top + ph};
        px += m.page_glyph_w + m.cand_spacing;
      }
      if (want_next) {
        out->next_page = FamoRect{px, page_row_top, px + m.page_glyph_w, page_row_top + ph};
        px += m.page_glyph_w;
      }
      int32_t page_w = px - x0;
      if (page_w > max_content_w) max_content_w = page_w;
      y = page_row_top + ph;
    }

    int32_t content_w = max_content_w + 2 * panel_margin_x;
    int32_t content_h = y + m.margin_y;
    out->content_size = ClampSize(m, content_w, content_h);
  }

  // Status-icon slot (top-right of the panel) when the skin reserves one.
  if (m.status_icon > 0) {
    int32_t s = m.status_icon;
    int32_t right = out->content_size.cx - panel_margin_x;
    out->status_icon = FamoRect{right - s, m.margin_y, right, m.margin_y + s};
  }

  // Drop-shadow margin (paint buffer = content + 2*margin; host offsets window by
  // -margin). Anchoring stays on content_size so the content lands at the caret.
  out->shadow_margin = ShadowMargin(sk, in.dpi);

  // Caret-follow + screen-edge flip → final origin.
  FamoComputeAnchor(&in.caret_rect, &in.work_area, out->content_size,
                    &out->origin_x, &out->origin_y, &out->flipped);

  return FAMO_UI_OK;
}

// FamoCandidateUiPaint + FamoTextResources* live in render/famo_paint.cpp (B5) —
// they pull in <windows.h>/D2D/DWrite/GDI+, kept out of this headless layout TU.
