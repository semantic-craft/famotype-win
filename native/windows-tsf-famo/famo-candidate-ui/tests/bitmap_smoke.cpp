// Headless bitmap smoke for FamoCandidateUiPaint (B5) — the real B5 gate.
//
// Renders a known view+skin to a DIB-backed memDC with the real DirectWrite
// measure + D2D/DWrite/GDI+ paint (no window — UpdateLayeredWindow compositing is
// the only real-machine step), reads the pixels back, and asserts the load-
// bearing behaviors: the highlight rect is filled with hilited_back_color, the
// highlighted candidate's text region has glyph pixels, a transparent-colored
// comment is NOT drawn, and the panel background is back_color.
//
// Colors are opaque so premultiplied == straight and we can compare RGB directly;
// we mask off alpha (readback alpha depends on the GDI/GDI+ path). CHECK returns
// exit 1 on first failure in any config (assert() is stripped under NDEBUG), like
// geometry_selfcheck / roundtrip_selfcheck.

#include <windows.h>

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

FamoUtf8String Str(const char* s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = s ? static_cast<uint32_t>(std::strlen(s)) : 0u;
  return v;
}
FamoUtf8String Empty() { return Str(nullptr); }

inline uint32_t Rgb(uint32_t argb) { return argb & 0x00FFFFFFu; }

// A 32-bit top-down DIB behind a memDC, sentinel-filled transparent.
struct Dib {
  HDC dc = nullptr;
  HBITMAP bm = nullptr;
  HGDIOBJ old = nullptr;
  uint32_t* px = nullptr;
  int w = 0, h = 0;
  bool Make(int cx, int cy) {
    w = cx; h = cy;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    bm = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bm || !bits) return false;
    dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    old = SelectObject(dc, bm);
    px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < cx * cy; ++i) px[i] = 0x00000000u;  // transparent sentinel
    return true;
  }
  uint32_t At(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
  ~Dib() {
    if (dc) { SelectObject(dc, old); DeleteDC(dc); }
    if (bm) DeleteObject(bm);
  }
};

}  // namespace

// A drop shadow (item 1): shadow_margin>0, the paint buffer is content+2*margin,
// the panel bg still lands at (margin,margin), and the margin ring carries shadow
// pixels (a blurred silhouette bleeds left of the opaque panel).
static int check_shadow() {
  FamoSkin sk = FamoSkinDefault();
  sk.layout_type = FAMO_LAYOUT_VERTICAL;
  sk.round_corner = 4;
  sk.border = 0;
  sk.back_color = 0xFF202020u;      // opaque dark panel
  sk.shadow_color = 0xFF00FF00u;    // opaque green shadow (distinct → easy to spot)
  sk.shadow_radius = 6;
  sk.shadow_offset_x = 0;
  sk.shadow_offset_y = 0;
  sk.prevpage_color = 0x00000000u;
  sk.nextpage_color = 0x00000000u;
  sk.comment_color = 0x00000000u;
  sk.hilited_comment_color = 0x00000000u;

  FamoCandidate cand[1];
  std::memset(cand, 0, sizeof(cand));
  cand[0].size = static_cast<uint32_t>(sizeof(FamoCandidate));
  cand[0].label = Str("1"); cand[0].text = Str("Aa"); cand[0].comment = Empty();

  FamoCompositionView v;
  std::memset(&v, 0, sizeof(v));
  v.size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  v.preedit = Empty(); v.commit = Empty();
  v.candidates = cand; v.candidate_count = 1;
  v.highlighted_index = 0; v.page_size = 5; v.is_last_page = 1;

  FamoTextResources* res = FamoTextResourcesCreate(&sk, 96);
  CHECK(res != nullptr);
  FamoLayoutInput in;
  std::memset(&in, 0, sizeof(in));
  in.size = static_cast<uint32_t>(sizeof(FamoLayoutInput));
  in.caret_rect = FamoRect{100, 100, 102, 120};
  in.work_area = FamoRect{0, 0, 1920, 1080};
  in.dpi = 96; in.aux = Empty();
  in.measure = &FamoTextMeasure; in.measure_user = res;

  FamoLayoutResult out;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
  const int32_t sm = out.shadow_margin;
  CHECK(sm > 0);  // shadow enabled → non-zero margin
  const int32_t cx = out.content_size.cx, cy = out.content_size.cy;
  const int32_t fcx = cx + 2 * sm, fcy = cy + 2 * sm;

  Dib dib;
  CHECK(dib.Make(fcx, fcy));
  CHECK(FamoCandidateUiPaint(&v, &sk, &in, &out, res, dib.dc) == FAMO_UI_OK);
  GdiFlush();

  // content bg still at the margin-shifted content origin (left inner strip).
  CHECK(Rgb(dib.At(sm + sk.margin_x / 2, sm + cy / 2)) == Rgb(sk.back_color));
  // shadow bled into the left margin ring (some non-transparent pixel there).
  bool shadow_here = false;
  for (int x = 1; x < sm && !shadow_here; ++x)
    if ((dib.At(x, fcy / 2) >> 24) != 0u) shadow_here = true;
  CHECK(shadow_here);

  FamoTextResourcesDestroy(res);
  return 0;
}

// Preedit stays quiet: a converting sub-range gets a thin DOTTED INK rule and
// the cursor gets a thin caret. Neither state may restore the old filled block,
// and neither may spend the accent on the rule — the accent belongs to the
// commit target (highlight pill) and the caret, so a third saturated use in the
// same panel is what made the old solid rule read as a spell-check error.
static int check_soft_cursor() {
  FamoSkin sk = FamoSkinDefault();
  sk.layout_type = FAMO_LAYOUT_VERTICAL;
  sk.round_corner = 0; sk.border = 0; sk.shadow_radius = 0;  // sm==0 → simple coords
  sk.back_color = 0xFF202020u;
  sk.hilited_back_color = 0xFFC02040u;   // distinct magenta for the preedit sel bg
  sk.text_color = 0xFFFFFFFFu; sk.hilited_text_color = 0xFF00FF00u;
  sk.candidate_text_color = 0xFFFFFFFFu; sk.label_color = 0xFFFFFFFFu;
  sk.comment_color = 0x00000000u; sk.hilited_comment_color = 0x00000000u;
  sk.prevpage_color = 0x00000000u; sk.nextpage_color = 0x00000000u;

  FamoCandidate cand[1];
  std::memset(cand, 0, sizeof(cand));
  cand[0].size = static_cast<uint32_t>(sizeof(FamoCandidate));
  cand[0].label = Str("1"); cand[0].text = Str("Aa"); cand[0].comment = Empty();

  FamoCompositionView v;
  std::memset(&v, 0, sizeof(v));
  v.size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  v.preedit = Str("faop");                 // ASCII → byte offset == wchar offset
  v.candidates = cand; v.candidate_count = 1; v.highlighted_index = 0;
  v.page_size = 5; v.is_last_page = 1;
  v.preedit_sel_start = 1; v.preedit_sel_end = 3;  // "ao" is the converting segment
  v.state_flags = FAMO_COMPOSITION_HAS_PREEDIT;

  FamoTextResources* res = FamoTextResourcesCreate(&sk, 96);
  CHECK(res != nullptr);
  FamoLayoutInput in;
  std::memset(&in, 0, sizeof(in));
  in.size = static_cast<uint32_t>(sizeof(FamoLayoutInput));
  in.caret_rect = FamoRect{100, 100, 102, 120};
  in.work_area = FamoRect{0, 0, 1920, 1080};
  in.dpi = 96; in.aux = Empty();
  in.measure = &FamoTextMeasure; in.measure_user = res;

  FamoLayoutResult out;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
  CHECK(out.preedit_sel_start_wchar == 1 && out.preedit_sel_end_wchar == 3);
  const int32_t cx = out.content_size.cx, cy = out.content_size.cy;

  auto accent_pixels_in_row = [&](const FamoLayoutResult& o, const Dib& d,
                                  int y) -> int {
    const FamoRect& pr = o.preedit;
    int count = 0;
    for (int x = pr.left; x < pr.right; ++x)
      if (Rgb(d.At(x, y)) == Rgb(sk.hilited_back_color)) ++count;
    return count;
  };
  auto max_accent_pixels = [&](const FamoLayoutResult& o, const Dib& d,
                               bool include_last_row) -> int {
    const FamoRect& pr = o.preedit;
    int maximum = 0;
    const int bottom = include_last_row ? pr.bottom : pr.bottom - 1;
    for (int y = pr.top; y < bottom; ++y)
      maximum = (std::max)(maximum, accent_pixels_in_row(o, d, y));
    return maximum;
  };

  {
    Dib dib;
    CHECK(dib.Make(cx, cy));
    CHECK(FamoCandidateUiPaint(&v, &sk, &in, &out, res, dib.dc) == FAMO_UI_OK);
    GdiFlush();
    CHECK(out.preedit.right > out.preedit.left);
    const int row = out.preedit.bottom - 1;
    // Scan the rule's OWN span, not the whole preedit row: "faop" puts a 'p'
    // descender in this row too, and its whitespace would hand a whole-row scan
    // a free "gap" — making the dotted assertion below pass on a solid bar.
    const int32_t before = FamoTextMeasure(res, 1, "f", 1);
    const int32_t active = FamoTextMeasure(res, 1, "ao", 2);
    CHECK(active > 4);
    const int rule_l = out.preedit.left + before, rule_r = rule_l + active;
    int inked = 0;
    for (int x = rule_l; x < rule_r; ++x)
      if (Rgb(dib.At(x, row)) != Rgb(sk.back_color)) ++inked;
    CHECK(inked > 2);          // the segment is marked at all
    CHECK(inked < active - 1);  // dotted, not the old solid bar (which inked all)
    // Accent is the caret's alone. Unlike before, the underline row is included
    // in the sweep — that inclusion is the assertion that the rule dropped it.
    CHECK(max_accent_pixels(out, dib, true) <= sk.caret_width);
  }
  // Control: no converting sub-range → only the thin vertical caret remains.
  v.preedit_sel_start = 0; v.preedit_sel_end = 0;
  v.preedit_cursor_pos = 2;
  FamoLayoutResult out0;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out0) == FAMO_UI_OK);
  CHECK(out0.preedit_cursor_wchar == 2);
  {
    Dib dib;
    CHECK(dib.Make(cx, cy));
    CHECK(FamoCandidateUiPaint(&v, &sk, &in, &out0, res, dib.dc) == FAMO_UI_OK);
    GdiFlush();
    CHECK(max_accent_pixels(out0, dib, true) == sk.caret_width);
  }
  // The caret honours FamoSkin.caret_width (host fills it from
  // SPI_GETCARETWIDTH). Widening the skin must widen the painted caret — this is
  // what keeps the Windows accessibility setting from being silently ignored.
  // Layout is unaffected (caret width is paint-only), so out0 is reused.
  {
    FamoSkin wide = sk;
    wide.caret_width = 4;
    FamoTextResources* wres = FamoTextResourcesCreate(&wide, 96);
    CHECK(wres != nullptr);
    Dib dib;
    CHECK(dib.Make(cx, cy));
    CHECK(FamoCandidateUiPaint(&v, &wide, &in, &out0, wres, dib.dc) == FAMO_UI_OK);
    GdiFlush();
    auto widest = [&](int y) {
      int count = 0;
      for (int x = out0.preedit.left; x < out0.preedit.right; ++x)
        if (Rgb(dib.At(x, y)) == Rgb(wide.hilited_back_color)) ++count;
      return count;
    };
    int maximum = 0;
    for (int y = out0.preedit.top; y < out0.preedit.bottom; ++y)
      maximum = (std::max)(maximum, widest(y));
    CHECK(maximum == 4);
    FamoTextResourcesDestroy(wres);
  }

  FamoTextResourcesDestroy(res);
  return 0;
}

int main() {
  // ── Skin: square (round_corner 0) + no border/shadow so pixels are exact.
  FamoSkin sk = FamoSkinDefault();
  sk.layout_type = FAMO_LAYOUT_VERTICAL;
  sk.round_corner = 0;
  sk.border = 0;
  sk.shadow_radius = 0;
  sk.back_color = 0xFF202020u;             // opaque dark gray panel
  sk.hilited_back_color = 0xFFC02040u;     // opaque magenta highlight (distinct)
  sk.hilited_text_color = 0xFFFFFFFFu;     // white
  sk.candidate_text_color = 0xFFFFFFFFu;
  sk.label_color = 0xFFFFFFFFu;
  sk.text_color = 0xFFFFFFFFu;
  sk.comment_color = 0x00000000u;          // TRANSPARENT → comment skipped
  sk.hilited_comment_color = 0x00000000u;
  sk.prevpage_color = 0x00000000u;         // no page glyphs (keep it simple)
  sk.nextpage_color = 0x00000000u;
  sk.margin_x = 12;
  sk.margin_y = 8;
  sk.hilite_padding_x = 10;
  sk.hilite_padding_y = 5;

  // ── View: 2 Latin candidates (Segoe UI has them → no fallback risk), cand 0
  // highlighted, both carry a comment string (skipped via transparent color).
  FamoCandidate cand[2];
  std::memset(cand, 0, sizeof(cand));
  for (int i = 0; i < 2; ++i) cand[i].size = static_cast<uint32_t>(sizeof(FamoCandidate));
  cand[0].label = Str("1"); cand[0].text = Str("Aa"); cand[0].comment = Str("note");
  cand[1].label = Str("2"); cand[1].text = Str("Bb"); cand[1].comment = Empty();

  FamoCompositionView v;
  std::memset(&v, 0, sizeof(v));
  v.size = static_cast<uint32_t>(sizeof(FamoCompositionView));
  v.preedit = Str("fa");
  v.commit = Empty();
  v.candidates = cand;
  v.candidate_count = 2;
  v.highlighted_index = 0;
  v.page_index = 0;
  v.page_size = 5;
  v.state_flags = FAMO_COMPOSITION_HAS_PREEDIT | FAMO_COMPOSITION_HAS_CANDIDATES;
  v.commit_preview = Empty();
  v.schema_id = Str("t");
  v.schema_name = Str("T");
  v.status_flags = FAMO_STATUS_COMPOSING;
  v.is_last_page = 1;

  // ── Real DirectWrite resources; wire the real measure into layout so the rects
  // match what paint draws.
  FamoTextResources* res = FamoTextResourcesCreate(&sk, 96);
  CHECK(res != nullptr);

  FamoLayoutInput in;
  std::memset(&in, 0, sizeof(in));
  in.size = static_cast<uint32_t>(sizeof(FamoLayoutInput));
  in.caret_rect = FamoRect{100, 100, 102, 120};
  in.work_area = FamoRect{0, 0, 1920, 1080};
  in.dpi = 96;
  in.aux = Empty();
  in.measure = &FamoTextMeasure;
  in.measure_user = res;

  FamoLayoutResult out;
  CHECK(FamoCandidateUiLayout(&v, &sk, &in, &out) == FAMO_UI_OK);
  const int32_t cx = out.content_size.cx, cy = out.content_size.cy;
  CHECK(cx > 0 && cy > 0);

  // ── 32-bit top-down DIB section behind a memDC.
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cx;
  bmi.bmiHeader.biHeight = -cy;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  CHECK(dib != nullptr && bits != nullptr);
  HDC memdc = CreateCompatibleDC(nullptr);
  CHECK(memdc != nullptr);
  HGDIOBJ oldbm = SelectObject(memdc, dib);

  // Sentinel-fill so a no-op paint would fail the background check outright.
  uint32_t* px = static_cast<uint32_t*>(bits);
  for (int i = 0; i < cx * cy; ++i) px[i] = 0x00010203u;

  const int32_t pr = FamoCandidateUiPaint(&v, &sk, &in, &out, res, memdc);
  CHECK(pr == FAMO_UI_OK);
  GdiFlush();  // ensure all batched GDI writes to the DIB landed

  auto at = [&](int x, int y) -> uint32_t { return px[static_cast<size_t>(y) * cx + x]; };

  // (d) panel background: the left margin strip (x < margin_x) is pure back_color.
  CHECK(Rgb(at(sk.margin_x / 2, cy / 2)) == Rgb(sk.back_color));

  // (a) highlight fill: top padding strip of the highlighted row (above the text).
  const FamoRect hb = out.highlight;
  CHECK(hb.right > hb.left && hb.bottom > hb.top);
  CHECK(Rgb(at((hb.left + hb.right) / 2, hb.top + 1)) == Rgb(sk.hilited_back_color));

  // (b) text drawn: some pixel in candidate 0's text rect differs from the
  // highlight background it sits on.
  const FamoRect tr = out.candidates[0].text;
  CHECK(tr.right > tr.left && tr.bottom > tr.top);
  bool text_pixel = false;
  for (int y = tr.top; y < tr.bottom && !text_pixel; ++y)
    for (int x = tr.left; x < tr.right; ++x)
      if (Rgb(at(x, y)) != Rgb(sk.hilited_back_color)) { text_pixel = true; break; }
  CHECK(text_pixel);

  // (c) transparent comment NOT drawn: the strip right of the text, inside the
  // highlight, stays highlight-colored (a drawn "note" would show white here).
  CHECK(out.candidates[0].has_comment == 0u);  // layout skipped it (color alpha 0)
  const int cxr = (tr.right + hb.right) / 2;
  const int cyr = (hb.top + hb.bottom) / 2;
  CHECK(Rgb(at(cxr, cyr)) == Rgb(sk.hilited_back_color));

  SelectObject(memdc, oldbm);
  DeleteDC(memdc);
  DeleteObject(dib);
  FamoTextResourcesDestroy(res);

  if (int r = check_shadow()) return r;
  if (int r = check_soft_cursor()) return r;

  std::printf("bitmap_smoke: OK (%dx%d) +shadow +soft-cursor\n", cx, cy);
  return 0;
}
