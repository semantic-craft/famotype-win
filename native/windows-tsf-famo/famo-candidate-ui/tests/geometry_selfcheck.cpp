// Headless geometry self-check for FamoCandidateUI (B4).
//
// Deterministic, device-free, CHECK-macro (assert() is stripped under NDEBUG /
// Release, which would pass vacuously — CHECK returns exit 1 on first failure in
// any config so ctest can trust EXIT=0). Mirrors engine-api/roundtrip_selfcheck.
//
// UTF-8 literals are explicit byte escapes so correctness does not depend on the
// compiler's source charset.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../famo_candidate_ui.h"

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      return 1;                                                           \
    }                                                                     \
  } while (0)

namespace {

// 你好  (2 CJK code points, 3 bytes each = 6 bytes, 2 UTF-16 units)
const char kNiHao[] = "\xE4\xBD\xA0\xE5\xA5\xBD";
// 你A好 = CJK, ascii, CJK  → bytes: 3+1+3 = 7; wchars: 1+1+1 = 3
const char kMixed[] = "\xE4\xBD\xA0\x41\xE5\xA5\xBD";
// 😀 U+1F600 = 4 UTF-8 bytes, 2 UTF-16 units (surrogate pair)
const char kEmoji[] = "\xF0\x9F\x98\x80";
// 你😀 = 3 + 4 bytes; wchars 1 + 2 = 3
const char kCjkEmoji[] = "\xE4\xBD\xA0\xF0\x9F\x98\x80";

FamoUtf8String Str(const char* s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = static_cast<uint32_t>(std::strlen(s));
  return v;
}

FamoUtf8String Empty() {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = nullptr;
  v.length_bytes = 0;
  return v;
}

FamoRect R(int32_t l, int32_t t, int32_t r, int32_t b) { return FamoRect{l, t, r, b}; }
int32_t W(const FamoRect& r) { return r.right - r.left; }
int32_t H(const FamoRect& r) { return r.bottom - r.top; }
bool NonEmpty(const FamoRect& r) { return W(r) > 0 && H(r) > 0; }
bool IsEmpty(const FamoRect& r) { return W(r) == 0 && H(r) == 0; }

// a fully inside b (inclusive edges).
bool Inside(const FamoRect& a, const FamoRect& b) {
  return a.left >= b.left && a.top >= b.top && a.right <= b.right &&
         a.bottom <= b.bottom;
}

// Deterministic monospace measurer: width = units(utf8) * 10 device px, ignoring
// font/DPI so the test math is exact and reproducible. Astral chars = 2 units.
int32_t Measure(void* /*user*/, int32_t /*which*/, const char* s, uint32_t len) {
  uint32_t units = 0;
  for (uint32_t i = 0; i < len;) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t n = 1;
    if (c < 0x80u) n = 1;
    else if ((c >> 5) == 0x6u) n = 2;
    else if ((c >> 4) == 0xEu) n = 3;
    else if ((c >> 3) == 0x1Eu) n = 4;
    if (i + n > len) n = len - i;
    if (n == 0) n = 1;
    units += (n >= 4) ? 2u : 1u;
    i += n;
  }
  return static_cast<int32_t>(units) * 10;
}

// Build a candidate array (caller owns storage). label/text/comment as given.
struct Cand {
  const char* label;
  const char* text;
  const char* comment;
};

void FillCands(FamoCandidate* arr, const Cand* src, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    std::memset(&arr[i], 0, sizeof(arr[i]));
    arr[i].size = static_cast<uint32_t>(sizeof(FamoCandidate));
    arr[i].label = src[i].label ? Str(src[i].label) : Empty();
    arr[i].text = Str(src[i].text);
    arr[i].comment = src[i].comment ? Str(src[i].comment) : Empty();
    arr[i].flags = (i == 0) ? FAMO_CANDIDATE_FLAG_DEFAULT : 0u;
  }
}

FamoLayoutInput MakeInput(FamoRect caret, FamoRect work) {
  FamoLayoutInput in;
  std::memset(&in, 0, sizeof(in));
  in.size = static_cast<uint32_t>(sizeof(FamoLayoutInput));
  in.caret_rect = caret;
  in.work_area = work;
  in.dpi = 96;  // no scaling → skin metrics == device px, easy exact asserts
  in.aux = Empty();
  in.measure = &Measure;
  in.measure_user = nullptr;
  return in;
}

// Shared 3-candidate view with a multibyte preedit and a highlighted middle one.
FamoCompositionView MakeView(FamoCandidate* arr, uint32_t n, uint32_t highlighted,
                             uint32_t page_index, uint32_t is_last_page) {
  FamoCompositionView v;
  std::memset(&v, 0, sizeof(v));
  v.size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  v.preedit = Str(kNiHao);
  v.commit = Empty();
  v.candidates = arr;
  v.candidate_count = n;
  v.highlighted_index = highlighted;
  v.page_index = page_index;
  v.page_size = 5;
  v.state_flags = FAMO_COMPOSITION_HAS_PREEDIT | FAMO_COMPOSITION_HAS_CANDIDATES;
  v.preedit_sel_start = 0;
  v.preedit_sel_end = 6;       // whole "你好" selected (byte offset)
  v.preedit_cursor_pos = 6;
  v.commit_preview = Empty();
  v.schema_id = Str("test");
  v.schema_name = Str("Test");
  v.status_flags = FAMO_STATUS_COMPOSING;
  v.is_last_page = is_last_page;
  return v;
}

// Common structural assertions for a laid-out result: content bounds candidates,
// per-candidate ordering, sub-rects inside their row.
int CheckCommon(const FamoLayoutResult& out, uint32_t n) {
  CHECK(out.content_size.cx > 0 && out.content_size.cy > 0);
  const FamoRect content = R(0, 0, out.content_size.cx, out.content_size.cy);
  CHECK(out.candidate_count == n);
  for (uint32_t i = 0; i < n; ++i) {
    const FamoCandidateRects& c = out.candidates[i];
    CHECK(NonEmpty(c.bounds));
    CHECK(Inside(c.bounds, content));           // candidate fits in the panel
    CHECK(NonEmpty(c.text));
    CHECK(Inside(c.text, c.bounds));            // text within its row
    if (NonEmpty(c.label)) CHECK(Inside(c.label, c.bounds));
    if (c.has_comment) {
      CHECK(NonEmpty(c.comment));
      CHECK(Inside(c.comment, c.bounds));
      // comment sits to the right of text within the row
      CHECK(c.comment.left >= c.text.right);
    }
    if (NonEmpty(c.label)) CHECK(c.label.right <= c.text.left);  // label before text
  }
  return 0;
}

int RunFamily(FamoLayoutType type) {
  Cand src[3] = {
      {"1", kNiHao, nullptr},
      {"2", kMixed, "\xE6\xB3\xA8"},  // comment 注
      {"3", kEmoji, nullptr},
  };
  FamoCandidate arr[3];
  FillCands(arr, src, 3);

  FamoSkin sk = FamoSkinDefault();
  sk.layout_type = type;
  // Force affordances + comment visible (opaque) regardless of default palette.
  sk.comment_color = 0xFF909090u;
  sk.prevpage_color = 0xFF808080u;
  sk.nextpage_color = 0xFF808080u;

  const FamoRect work = R(0, 0, 1920, 1080);
  const FamoRect caret = R(400, 300, 402, 320);  // mid-screen → no flip
  FamoLayoutInput in = MakeInput(caret, work);

  // ── Case: middle page (page_index=1, not last) → BOTH < and > present ──
  {
    FamoCompositionView v = MakeView(arr, 3, /*hl*/ 1, /*page*/ 1, /*last*/ 0);
    FamoLayoutResult out;
    CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
    if (CheckCommon(out, 3)) return 1;
    CHECK(NonEmpty(out.preedit));                 // multibyte preedit band shown
    CHECK(NonEmpty(out.highlight));
    if (type == FAMO_LAYOUT_HORIZONTAL) {
      CHECK(out.highlight.left == out.candidates[1].bounds.left);
      CHECK(out.highlight.right == out.candidates[1].bounds.right);
      CHECK(out.highlight.top == out.candidates[1].bounds.top - 4);
      CHECK(out.highlight.bottom ==
            (std::min)(out.content_size.cy,
                       out.candidates[1].bounds.bottom + 9));
    } else {
      CHECK(std::memcmp(&out.highlight, &out.candidates[1].bounds,
                        sizeof(FamoRect)) == 0);
    }
    CHECK(NonEmpty(out.prev_page));               // page_index>0 → "<"
    CHECK(NonEmpty(out.next_page));               // !is_last_page → ">"

    // Candidate ordering + non-overlap along the family's stacking axis.
    for (uint32_t i = 0; i + 1 < 3; ++i) {
      const FamoRect& a = out.candidates[i].bounds;
      const FamoRect& b = out.candidates[i + 1].bounds;
      if (type == FAMO_LAYOUT_HORIZONTAL) {
        CHECK(b.left >= a.right);                  // left→right, no overlap
      } else {
        CHECK(b.top >= a.bottom);                  // top→bottom, no overlap
      }
    }
    // Preedit band is above the candidate list.
    CHECK(out.candidates[0].bounds.top >= out.preedit.bottom);
  }

  // ── Case: first + last page (page_index=0, last) → NEITHER < nor > ──
  {
    FamoCompositionView v = MakeView(arr, 3, /*hl*/ 0, /*page*/ 0, /*last*/ 1);
    FamoLayoutResult out;
    CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
    if (CheckCommon(out, 3)) return 1;
    CHECK(IsEmpty(out.prev_page));                // page_index==0 → no "<"
    CHECK(IsEmpty(out.next_page));                // is_last_page → no ">"
    if (type == FAMO_LAYOUT_HORIZONTAL) {
      CHECK(out.highlight.top == out.candidates[0].bounds.top - 4);
      CHECK(out.highlight.bottom ==
            (std::min)(out.content_size.cy,
                       out.candidates[0].bounds.bottom + 9));
    } else {
      CHECK(std::memcmp(&out.highlight, &out.candidates[0].bounds,
                        sizeof(FamoRect)) == 0);
    }
  }

  return 0;
}

int RunFlip() {
  FamoSkin sk = FamoSkinDefault();
  const FamoRect work = R(0, 0, 1920, 1080);

  // Content 200x100.
  FamoSize content{200, 100};

  // Caret near TOP → below-caret fits → NOT flipped, y = caret.bottom + 6.
  {
    FamoRect caret = R(500, 50, 502, 70);
    int32_t x, y;
    uint32_t flipped;
    FamoComputeAnchor(&caret, &work, content, &x, &y, &flipped);
    CHECK(flipped == 0);
    CHECK(y == 70 + 6);
    CHECK(x == 500);
  }
  // Caret near BOTTOM (bottom+6+100 > 1080) → flip above, y = caret.top-100-6.
  {
    FamoRect caret = R(500, 1040, 502, 1060);
    int32_t x, y;
    uint32_t flipped;
    FamoComputeAnchor(&caret, &work, content, &x, &y, &flipped);
    CHECK(flipped == 1);
    CHECK(y == 1040 - 100 - 6);
  }
  // Caret near RIGHT edge → x clamped to work.right - width.
  {
    FamoRect caret = R(1900, 300, 1902, 320);
    int32_t x, y;
    uint32_t flipped;
    FamoComputeAnchor(&caret, &work, content, &x, &y, &flipped);
    CHECK(x == 1920 - 200);   // clamped
    CHECK(flipped == 0);
  }
  // Caret near LEFT (negative) → x clamped to work.left.
  {
    FamoRect caret = R(-30, 300, -28, 320);
    int32_t x, y;
    uint32_t flipped;
    FamoComputeAnchor(&caret, &work, content, &x, &y, &flipped);
    CHECK(x == 0);
  }
  // A layout call carries the same flip into the result (integration).
  {
    Cand src[1] = {{"1", kNiHao, nullptr}};
    FamoCandidate arr[1];
    FillCands(arr, src, 1);
    FamoRect caret = R(500, 1040, 502, 1060);
    FamoLayoutInput in = MakeInput(caret, work);
    FamoCompositionView v = MakeView(arr, 1, 0, 0, 1);
    FamoLayoutResult out;
    CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
    CHECK(out.flipped == 1);
    CHECK(out.origin_y == caret.top - out.content_size.cy - 6);
  }
  return 0;
}

int RunOffsetConversion() {
  // Pure helper: UTF-8 byte offset → UTF-16 wchar offset.
  // "你好": bytes 0,3,6 → wchars 0,1,2
  CHECK(FamoUtf8ByteToWchar(kNiHao, 6, 0) == 0);
  CHECK(FamoUtf8ByteToWchar(kNiHao, 6, 3) == 1);
  CHECK(FamoUtf8ByteToWchar(kNiHao, 6, 6) == 2);
  // "你A好": bytes 0,3,4,7 → wchars 0,1,2,3
  CHECK(FamoUtf8ByteToWchar(kMixed, 7, 3) == 1);
  CHECK(FamoUtf8ByteToWchar(kMixed, 7, 4) == 2);
  CHECK(FamoUtf8ByteToWchar(kMixed, 7, 7) == 3);
  // Emoji 😀 alone: 4 bytes → 2 wchars (surrogate pair).
  CHECK(FamoUtf8ByteToWchar(kEmoji, 4, 4) == 2);
  CHECK(FamoUtf8ByteToWchar(kEmoji, 4, 0) == 0);
  // "你😀": byte 0,3,7 → wchar 0,1,3 (CJK=1, astral=2)
  CHECK(FamoUtf8ByteToWchar(kCjkEmoji, 7, 3) == 1);
  CHECK(FamoUtf8ByteToWchar(kCjkEmoji, 7, 7) == 3);
  // Out-of-range byte offset clamps to full wchar length.
  CHECK(FamoUtf8ByteToWchar(kCjkEmoji, 7, 99) == 3);

  // And the layout wires it into the result: whole "你好" selected → wchar 0..2.
  Cand src[1] = {{"1", kNiHao, nullptr}};
  FamoCandidate arr[1];
  FillCands(arr, src, 1);
  FamoSkin sk = FamoSkinDefault();
  FamoRect work = R(0, 0, 1920, 1080);
  FamoRect caret = R(400, 300, 402, 320);
  FamoLayoutInput in = MakeInput(caret, work);
  FamoCompositionView v = MakeView(arr, 1, 0, 0, 1);
  FamoLayoutResult out;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
  CHECK(out.preedit_sel_start_wchar == 0);
  CHECK(out.preedit_sel_end_wchar == 2);
  CHECK(out.preedit_cursor_wchar == 2);

  v.preedit = Str(kMixed);
  v.preedit_sel_start = 0;
  v.preedit_sel_end = 0;
  v.preedit_cursor_pos = 4;  // after "你A": byte 4 → wchar 2
  FamoLayoutResult out_cursor;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out_cursor) == FAMO_UI_OK);
  CHECK(out_cursor.preedit_sel_start_wchar == 0);
  CHECK(out_cursor.preedit_sel_end_wchar == 0);
  CHECK(out_cursor.preedit_cursor_wchar == 2);
  return 0;
}

int RunPreeditAndPreview() {
  Cand main_source[2] = {{"1", kNiHao, nullptr},
                         {"2", kMixed, nullptr}};
  Cand preview_source[4] = {{nullptr, kEmoji, nullptr},
                            {nullptr, kNiHao, nullptr},
                            {nullptr, kMixed, nullptr},
                            {nullptr, kCjkEmoji, nullptr}};
  FamoCandidate main_candidates[2];
  FamoCandidate preview_candidates[4];
  FillCands(main_candidates, main_source, 2);
  FillCands(preview_candidates, preview_source, 4);

  FamoSkin skin = FamoSkinDefault();
  skin.layout_type = FAMO_LAYOUT_HORIZONTAL;
  skin.preview_pages = 1;
  skin.preview_rows = 2;
  FamoLayoutInput input =
      MakeInput(R(400, 300, 402, 320), R(0, 0, 1920, 1080));
  input.preview_candidates = preview_candidates;
  input.preview_candidate_count = 4;
  input.preview_page_size = 2;
  FamoCompositionView view = MakeView(main_candidates, 2, 0, 0, 0);

  FamoLayoutResult shown{};
  CHECK(FamoCandidateUiLayout(&view, &skin, &input, &shown) == FAMO_UI_OK);
  CHECK(NonEmpty(shown.preedit));
  CHECK(shown.preview_candidate_count == 4);
  CHECK(shown.preview_candidates[2].bounds.top >
        shown.preview_candidates[0].bounds.top);
  CHECK(shown.highlight.top == shown.candidates[0].bounds.top - 4);
  CHECK(shown.highlight.bottom == shown.candidates[0].bounds.bottom + 2);

  skin.show_preedit = 0;
  FamoLayoutResult hidden{};
  CHECK(FamoCandidateUiLayout(&view, &skin, &input, &hidden) == FAMO_UI_OK);
  CHECK(IsEmpty(hidden.preedit));
  CHECK(hidden.content_size.cy < shown.content_size.cy);

  skin.layout_type = FAMO_LAYOUT_VERTICAL;
  FamoLayoutResult vertical{};
  CHECK(FamoCandidateUiLayout(&view, &skin, &input, &vertical) == FAMO_UI_OK);
  CHECK(vertical.preview_candidate_count == 0);
  CHECK(vertical.candidates[0].bounds.right ==
        vertical.candidates[1].bounds.right);
  return 0;
}

int RunArgGuards() {
  FamoSkin sk = FamoSkinDefault();
  FamoLayoutInput in = MakeInput(R(0, 0, 2, 20), R(0, 0, 1920, 1080));
  FamoCandidate arr[1];
  Cand src[1] = {{"1", kNiHao, nullptr}};
  FillCands(arr, src, 1);
  FamoCompositionView v = MakeView(arr, 1, 0, 0, 1);
  FamoLayoutResult out;
  CHECK(FamoCandidateUiLayout(nullptr, &sk, &in, &out) == FAMO_UI_E_INVALID_ARGUMENT);
  CHECK(FamoCandidateUiLayout(&v, nullptr, &in, &out) == FAMO_UI_E_INVALID_ARGUMENT);
  CHECK(FamoCandidateUiLayout(&v, &sk, nullptr, &out) == FAMO_UI_E_INVALID_ARGUMENT);
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, nullptr) == FAMO_UI_E_INVALID_ARGUMENT);
  // Paint (B5) needs a device → covered by the bitmap_smoke ctest, not here.
  return 0;
}

}  // namespace

int main() {
  if (RunFamily(FAMO_LAYOUT_VERTICAL)) return 1;
  if (RunFamily(FAMO_LAYOUT_HORIZONTAL)) return 1;
  if (RunFamily(FAMO_LAYOUT_VERTICAL_TEXT)) return 1;
  if (RunFlip()) return 1;
  if (RunOffsetConversion()) return 1;
  if (RunPreeditAndPreview()) return 1;
  if (RunArgGuards()) return 1;
  std::printf("geometry_selfcheck: OK\n");
  return 0;
}
