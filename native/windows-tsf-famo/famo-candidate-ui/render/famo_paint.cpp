// FamoCandidateUI paint pipeline (B5) — real device rendering.
//
// Clean-room: authored from the behavioral spec (research §1.5 = "offscreen-
// composited translucent popup, round corners + drop shadow, crisp DirectWrite
// text, color-scheme palette") + design §6, NOT from any upstream *Panel.cpp. The
// tech is ours (research §1.5 explicitly leaves it open): GDI+ for the shapes,
// Direct2D/DirectWrite for the text, composited into one 32-bit premultiplied
// bitmap the host later blits with UpdateLayeredWindow.
//
// Split from the headless layout TU on purpose: everything <windows.h>/D2D/
// DWrite/GDI+ lives here. Layout stays pure math, unit-tested device-free; this
// file is exercised by the bitmap_smoke ctest (DIB-backed memDC → pixel readback).
//
// GDI+ shapes and D2D text cannot safely share one DC render target (a DC render
// target does not reliably preserve the DC's prior GDI+ content across BeginDraw/
// EndDraw), so text is drawn to its own transparent DIB with D2D and then
// alpha-composited over the shapes with GDI+. One bitmap out, either way.

#include <windows.h>
#include <objidl.h>   // IStream — required before <gdiplus.h>
// GDI+ 1.1 (Vista+) gates the effect classes (Gdiplus::Blur / Bitmap::ApplyEffect)
// behind GDIPVER >= 0x0110 — the drop shadow's native gaussian. Must precede
// <gdiplus.h>. This TU compiles NotUsing-PCH in both the ctest and WeaselUI builds,
// so the define reliably reaches the gdiplus headers here.
#define GDIPVER 0x0110
#include <gdiplus.h>
#include <gdipluseffects.h>  // Gdiplus::Blur — native gaussian for the drop shadow
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>  // offsetof — FamoSkin size negotiation for caret_width
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../famo_candidate_access.h"
#include "../famo_candidate_ui.h"

#ifdef FAMO_CANDIDATE_UI_BENCHMARK_COUNTERS
#include "../benchmark/famo_benchmark_counters.h"
#endif

// Two SDK-macro traps, both left ALONE on purpose:
//  • <winuser.h> #defines DrawText → DrawTextW. Because <d2d1.h> is included with
//    that macro active, ID2D1RenderTarget's DrawText method is declared as
//    DrawTextW too — so our rt->DrawText(...) call expands the same way and
//    matches. Do NOT #undef DrawText: that desyncs the call from the (already
//    expanded) declaration → C2039.
//  • do NOT define NOMINMAX — GDI+ headers depend on the min/max macros.

using Microsoft::WRL::ComPtr;

// ── FamoTextResources: shared DirectWrite/D2D/GDI+ state ──────────────────────
struct FamoTextResources {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IDWriteTextFormat> fmt[3];  // 0=label 1=text 2=comment
  // Device resources, not cached scene content: BindDC retargets them each frame.
  ComPtr<ID2D1DCRenderTarget> text_target;
  ComPtr<ID2D1SolidColorBrush> text_brush;
  HDC text_dc = nullptr;
  HBITMAP text_bitmap = nullptr;
  HGDIOBJ text_previous = nullptr;
  void* text_bits = nullptr;
  int text_width = 0;
  int text_height = 0;
  std::vector<uint32_t> shadow_pixels;
  int shadow_width = 0;
  int shadow_height = 0;
  ULONG_PTR gdiplus_token = 0;
};

namespace {

#ifdef FAMO_CANDIDATE_UI_BENCHMARK_COUNTERS
FamoBenchmarkRenderCounters g_benchmark_counters;
#define FAMO_BENCHMARK_COUNT(field) (++g_benchmark_counters.field)
#else
#define FAMO_BENCHMARK_COUNT(field) ((void)0)
#endif

// ── small helpers ────────────────────────────────────────────────────────────

void ReleaseTextSurface(FamoTextResources* res) {
  if (res->text_dc) {
    if (res->text_previous) SelectObject(res->text_dc, res->text_previous);
    DeleteDC(res->text_dc);
  }
  if (res->text_bitmap) DeleteObject(res->text_bitmap);
  res->text_dc = nullptr;
  res->text_bitmap = nullptr;
  res->text_previous = nullptr;
  res->text_bits = nullptr;
  res->text_width = 0;
  res->text_height = 0;
}

bool EnsureTextSurface(FamoTextResources* res, HDC reference, int width,
                       int height) {
  if (res->text_dc && res->text_bitmap && res->text_bits &&
      res->text_width == width && res->text_height == height)
    return true;
  ReleaseTextSurface(res);

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) {
    if (bitmap) DeleteObject(bitmap);
    return false;
  }
  HDC dc = CreateCompatibleDC(reference);
  if (!dc) {
    DeleteObject(bitmap);
    return false;
  }
  HGDIOBJ previous = SelectObject(dc, bitmap);
  if (!previous || previous == HGDI_ERROR) {
    DeleteDC(dc);
    DeleteObject(bitmap);
    return false;
  }
  res->text_dc = dc;
  res->text_bitmap = bitmap;
  res->text_previous = previous;
  res->text_bits = bits;
  res->text_width = width;
  res->text_height = height;
  FAMO_BENCHMARK_COUNT(text_surface);
  return true;
}

void CompositePremultiplied(uint32_t* target, int target_stride,
                            const uint32_t* source, int width, int height) {
  for (int y = 0; y < height; ++y) {
    uint32_t* destination = target + static_cast<size_t>(y) * target_stride;
    const uint32_t* overlay = source + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      const uint32_t src = overlay[x];
      const uint32_t alpha = src >> 24;
      if (alpha == 0) continue;
      if (alpha == 255) {
        destination[x] = src;
        continue;
      }
      const uint32_t dst = destination[x];
      const uint32_t inverse = 255 - alpha;
      const auto blend = [inverse](uint32_t src_channel,
                                   uint32_t dst_channel) {
        return (std::min)(255u,
                          src_channel + (dst_channel * inverse + 127) / 255);
      };
      destination[x] =
          (blend((src >> 24) & 0xff, (dst >> 24) & 0xff) << 24) |
          (blend((src >> 16) & 0xff, (dst >> 16) & 0xff) << 16) |
          (blend((src >> 8) & 0xff, (dst >> 8) & 0xff) << 8) |
          blend(src & 0xff, dst & 0xff);
    }
  }
}

inline bool Opaque(uint32_t argb) { return (argb >> 24) != 0u; }  // alpha != 0
inline bool RectEmpty2(const FamoRect& r) {
  return r.right <= r.left || r.bottom <= r.top;
}

bool SkinResourceInputsValid(const FamoSkin* skin) {
  if (!skin || skin->size < offsetof(FamoSkin, caret_width)) return false;
  return std::memchr(skin->label_font.face, '\0', FAMO_FONT_FACE_MAX) &&
         std::memchr(skin->text_font.face, '\0', FAMO_FONT_FACE_MAX) &&
         std::memchr(skin->comment_font.face, '\0', FAMO_FONT_FACE_MAX);
}

bool CandidatePaintInputsValid(const FamoCompositionView* view,
                               const FamoSkin* skin,
                               const FamoLayoutInput* input,
                               const FamoLayoutResult* layout) {
  if (view->size < offsetof(FamoCompositionView, preedit_sel_start) ||
      skin->size < offsetof(FamoSkin, caret_width) ||
      input->size < offsetof(FamoLayoutInput, preview_candidates) ||
      layout->size <
          offsetof(FamoLayoutResult, flipped) + sizeof(layout->flipped))
    return false;
  if (!famo_candidate_ui::ReadableUtf8(view->preedit) ||
      !famo_candidate_ui::ReadableUtf8(input->aux) ||
      layout->candidate_count > FAMO_MAX_LAID_CANDIDATES ||
      layout->candidate_count > view->candidate_count ||
      (view->candidate_count > 0 && !view->candidates))
    return false;

  for (uint32_t i = 0; i < layout->candidate_count; ++i) {
    FamoCandidate candidate{};
    if (!famo_candidate_ui::ReadCandidate(
            view->candidates, view->candidate_count, i, &candidate) ||
        !famo_candidate_ui::ReadableCandidate(candidate))
      return false;
  }

  const bool has_preview_fields =
      input->size >= offsetof(FamoLayoutInput, preview_page_size) +
                         sizeof(input->preview_page_size);
  if (has_preview_fields && input->preview_candidate_count > 0 &&
      !input->preview_candidates)
    return false;
  if (layout->preview_candidate_count > FAMO_MAX_PREVIEW_CANDIDATES)
    return false;
  if (layout->preview_candidate_count > 0) {
    if (!has_preview_fields || !input->preview_candidates ||
        layout->preview_candidate_count > input->preview_candidate_count)
      return false;
    for (uint32_t i = 0; i < layout->preview_candidate_count; ++i) {
      FamoCandidate candidate{};
      if (!famo_candidate_ui::ReadCandidate(
              input->preview_candidates, input->preview_candidate_count, i,
              &candidate) ||
          !famo_candidate_ui::ReadableCandidate(candidate))
        return false;
    }
  }
  return true;
}

// Skin metric (logical px @96) → device px, dpi/96 round-to-nearest (matches the
// layout engine's Scale so shapes line up with the laid-out rects).
inline int32_t Scale(int32_t v, uint32_t dpi) {
  if (dpi == 0) dpi = 96;
  return static_cast<int32_t>((static_cast<int64_t>(v) * dpi + 48) / 96);
}

D2D1_COLOR_F ToColorF(uint32_t argb) {
  return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                      (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
}
Gdiplus::Color ToGdiColor(uint32_t argb) {
  return Gdiplus::Color((argb >> 24) & 0xFF, (argb >> 16) & 0xFF,
                        (argb >> 8) & 0xFF, argb & 0xFF);
}

std::wstring Widen(const char* utf8, uint32_t len) {
  std::wstring w;
  if (!utf8 || len == 0) return w;
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), nullptr, 0);
  if (n <= 0) return w;
  w.resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), &w[0], n);
  return w;
}

// A round-rect GraphicsPath (radius 0 → plain rectangle). Right/bottom exclusive
// FamoRect → GDI+ Rect width/height.
void AddRoundRect(Gdiplus::GraphicsPath& path, int x, int y, int w, int h, int r) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r <= 0) {
    path.AddRectangle(Gdiplus::Rect(x, y, w, h));
    return;
  }
  int d = r * 2;
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

// Build one IDWriteTextFormat from a FamoFontSpec. Font size is carried in device
// px (point * dpi/72) and the render target runs at 96 DPI (DIP==px), so measured
// and drawn advances are both device px — consistent with the layout engine.
// ponytail: a "face1, face2" fallback list keeps only face1; DirectWrite custom
// font-fallback chains are a real-machine refinement (research §1.4).
HRESULT MakeFormat(IDWriteFactory* dw, const FamoFontSpec& spec, uint32_t dpi,
                   IDWriteTextFormat** out) {
  std::wstring face = Widen(spec.face, static_cast<uint32_t>(strlen(spec.face)));
  size_t comma = face.find(L',');
  if (comma != std::wstring::npos) face.resize(comma);
  while (!face.empty() && face.back() == L' ') face.pop_back();
  if (face.empty()) face = L"Segoe UI";
  float px = spec.point_size * static_cast<float>(dpi) / 72.0f;
  if (px < 1.0f) px = 1.0f;
  HRESULT hr = dw->CreateTextFormat(face.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    px, L"", out);
  // 候选窗文本恒为单行：关掉自动换行。否则测量宽与绘制框宽因取整差 1px 时，含空格的串
  //（如拼音 preedit "ni hao"）会在空格处折行、溢出到候选行（S22 真机现象）。
  if (SUCCEEDED(hr) && *out)
    (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return hr;
}

// Measure a wide substring's advance in device px with a given format (0 on any
// error). Used to split the preedit at the highlighted (converting) sub-range.
int32_t MeasureW(IDWriteFactory* dw, IDWriteTextFormat* fmt, const wchar_t* s,
                 uint32_t len) {
  if (!dw || !fmt || !s || len == 0) return 0;
  ComPtr<IDWriteTextLayout> tl;
  if (FAILED(dw->CreateTextLayout(s, len, fmt, 1e6f, 1e6f, &tl))) return 0;
  FAMO_BENCHMARK_COUNT(text_layout);
  DWRITE_TEXT_METRICS tm = {};
  if (FAILED(tl->GetMetrics(&tm))) return 0;
  return static_cast<int32_t>(tm.widthIncludingTrailingWhitespace + 0.5f);
}

// Draw one wide string into a rect with a color, on the D2D render target. The
// rect is in content coords; (tx,ty) translates it into the shadow-margin-offset
// content region of the buffer. Not clipped to the rect (glyphs may overhang
// slightly — layout sizes rects to the same DWrite advance, so overhang is
// negligible). VerticalText rotates glyphs 90° about the rect center (ponytail:
// first-cut; the box geometry is still Vertical's — true vertical-text columns are
// tuned on real hardware, journal S17).
void DrawRun(ID2D1DCRenderTarget* rt, IDWriteTextFormat* fmt,
             ID2D1SolidColorBrush* brush, const wchar_t* s, uint32_t len,
             const FamoRect& rc, bool vertical, float tx, float ty) {
  if (!fmt || !s || len == 0 || RectEmpty2(rc)) return;
  D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(tx, ty);
  if (vertical) {
    D2D1_POINT_2F c = D2D1::Point2F((rc.left + rc.right) / 2.0f,
                                    (rc.top + rc.bottom) / 2.0f);
    base = D2D1::Matrix3x2F::Rotation(90.0f, c) * base;
  }
  rt->SetTransform(base);
  D2D1_RECT_F box = D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                static_cast<float>(rc.right), static_cast<float>(rc.bottom));
  rt->DrawText(s, len, fmt, box, brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawUtf8(ID2D1DCRenderTarget* rt, IDWriteTextFormat* fmt,
              ID2D1SolidColorBrush* brush, const FamoUtf8String& s,
              const FamoRect& rc, bool vertical, float tx, float ty) {
  if (!s.data || s.length_bytes == 0) return;
  std::wstring w = Widen(s.data, s.length_bytes);
  DrawRun(rt, fmt, brush, w.c_str(), static_cast<uint32_t>(w.size()), rc, vertical, tx, ty);
}

// ── the actual paint (C++ objects with destructors → kept out of the SEH frame) ─
int32_t PaintImpl(const FamoCompositionView* view, const FamoSkin* skin,
                  const FamoLayoutInput* input, const FamoLayoutResult* layout,
                  FamoTextResources* res, HDC hdc) try {
  const int32_t cx = layout->content_size.cx;
  const int32_t cy = layout->content_size.cy;
  if (cx <= 0 || cy <= 0) return FAMO_UI_E_INVALID_ARGUMENT;

  // The DC must carry a 32-bit DIB section — we draw GDI+ into its raw premul
  // buffer (GDI+ through an HDC zeroes the alpha channel; the raw buffer keeps it).
  HBITMAP hbm = static_cast<HBITMAP>(GetCurrentObject(hdc, OBJ_BITMAP));
  DIBSECTION ds = {};
  if (!hbm || GetObjectW(hbm, sizeof(ds), &ds) != sizeof(ds) ||
      ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
    return FAMO_UI_E_INVALID_ARGUMENT;  // needs a 32-bit top-down DIB section
  }
  const uint32_t dpi = input->dpi ? input->dpi : 96u;
  const int rc_dev = Scale(skin->round_corner, dpi);
  const int highlight_rc_dev = (std::max)(0, rc_dev - Scale(4, dpi));
  const int border_dev = Scale(skin->border, dpi);
  const bool vertical = skin->layout_type == FAMO_LAYOUT_VERTICAL_TEXT;
  const bool vertical_list = skin->layout_type != FAMO_LAYOUT_HORIZONTAL;

  // Drop-shadow margin: content is drawn at (sm,sm); the buffer is content+2*sm
  // (host sizes the DIB to match) with the gaussian shadow in the ring. sm==0 → no
  // shadow, content at the buffer origin (pre-shadow behavior).
  const int sm = layout->shadow_margin > 0 ? layout->shadow_margin : 0;
  const int fcx = cx + 2 * sm;
  const int fcy = cy + 2 * sm;
  if (ds.dsBm.bmWidth != fcx || ds.dsBm.bmHeight != fcy)
    return FAMO_UI_E_INVALID_ARGUMENT;
  const float sm_f = static_cast<float>(sm);

  // ── 1. GDI+ shapes into the DC's premultiplied buffer ───────────────────────
  {
    Gdiplus::Bitmap surface(fcx, fcy, ds.dsBm.bmWidthBytes, PixelFormat32bppPARGB,
                            static_cast<BYTE*>(ds.dsBm.bmBits));
    const size_t row_bytes = static_cast<size_t>(fcx) * sizeof(uint32_t);
    auto* target = static_cast<BYTE*>(ds.dsBm.bmBits);
    for (int y = 0; y < fcy; ++y)
      std::memset(target + static_cast<size_t>(y) * ds.dsBm.bmWidthBytes, 0,
                  row_bytes);

    // Drop shadow (native GDI+ gaussian): a blurred round-rect silhouette of the
    // panel, offset by shadow_offset, composited under the panel in the margin
    // ring. Guarded so any failure degrades to no-shadow, never a failed paint.
    if (sm > 0 && Opaque(skin->shadow_color)) {
      const int ox = Scale(skin->shadow_offset_x, dpi);
      const int oy = Scale(skin->shadow_offset_y, dpi);
      if (res->shadow_pixels.empty() || res->shadow_width != fcx ||
          res->shadow_height != fcy) {
        res->shadow_pixels.assign(static_cast<size_t>(fcx) * fcy, 0u);
        Gdiplus::Bitmap shadow(
            fcx, fcy, static_cast<INT>(row_bytes), PixelFormat32bppPARGB,
            reinterpret_cast<BYTE*>(res->shadow_pixels.data()));
        if (shadow.GetLastStatus() == Gdiplus::Ok) {
          {
            Gdiplus::Graphics sg(&shadow);
            sg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            sg.Clear(Gdiplus::Color(0, 0, 0, 0));
            Gdiplus::GraphicsPath sp;
            AddRoundRect(sp, sm + ox, sm + oy, cx, cy, rc_dev);
            Gdiplus::SolidBrush sb(ToGdiColor(skin->shadow_color));
            sg.FillPath(&sb, &sp);
          }
          float br = static_cast<float>(Scale(skin->shadow_radius, dpi));
          if (br > 255.0f) br = 255.0f;
          Gdiplus::Blur blur;
          Gdiplus::BlurParams bp = {br, FALSE};
          // Un-blurred silhouette is an acceptable fallback if the effect is absent.
          if (blur.SetParameters(&bp) == Gdiplus::Ok)
            shadow.ApplyEffect(&blur, nullptr);
          res->shadow_width = fcx;
          res->shadow_height = fcy;
        } else {
          res->shadow_pixels.clear();
        }
      }
      if (!res->shadow_pixels.empty() && res->shadow_width == fcx &&
          res->shadow_height == fcy) {
        const auto* shadow = reinterpret_cast<const BYTE*>(
            res->shadow_pixels.data());
        for (int y = 0; y < fcy; ++y)
          std::memcpy(target + static_cast<size_t>(y) * ds.dsBm.bmWidthBytes,
                      shadow + static_cast<size_t>(y) * row_bytes, row_bytes);
      }
    }

    Gdiplus::Graphics g(&surface);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Panel content is drawn in content coords, shifted into the shadow margin.
    g.TranslateTransform(sm_f, sm_f);
    if (Opaque(skin->back_color)) {
      Gdiplus::GraphicsPath path;
      AddRoundRect(path, 0, 0, cx, cy, rc_dev);
      Gdiplus::SolidBrush brush(ToGdiColor(skin->back_color));
      g.FillPath(&brush, &path);
    }
    // Derive the band → list hairline from stable layout fields. The public
    // output has no caller-capacity parameter, so carrying an appended rect
    // there would make a current Layout overwrite an older caller's buffer.
    const int32_t band_bottom =
        (std::max)(layout->preedit.bottom, layout->aux.bottom);
    FamoRect separator{};
    if (band_bottom > 0 && layout->candidate_count > 0) {
      const int32_t list_top = layout->candidates[0].bounds.top;
      if (list_top > band_bottom) {
        const int32_t top = band_bottom + (list_top - band_bottom) / 2;
        separator = {0, top, cx, top + (std::max)(1, Scale(1, dpi))};
      }
    }
    // Clip to the panel path so the full-bleed rule cannot poke out of a large
    // corner radius on a short panel.
    if (!RectEmpty2(separator) && Opaque(skin->border_color)) {
      const FamoRect& s = separator;
      Gdiplus::GraphicsPath panel;
      AddRoundRect(panel, 0, 0, cx, cy, rc_dev);
      Gdiplus::SolidBrush brush(ToGdiColor(skin->border_color));
      const Gdiplus::GraphicsState saved = g.Save();
      g.SetClip(&panel);
      g.FillRectangle(&brush, s.left, s.top, s.right - s.left, s.bottom - s.top);
      g.Restore(saved);
    }
    if (!RectEmpty2(layout->highlight) && Opaque(skin->hilited_back_color)) {
      const FamoRect& h = layout->highlight;
      Gdiplus::GraphicsPath path;
      AddRoundRect(path, h.left, h.top, h.right - h.left, h.bottom - h.top,
                   highlight_rc_dev);
      Gdiplus::SolidBrush brush(ToGdiColor(skin->hilited_back_color));
      g.FillPath(&brush, &path);
    }
    if (border_dev > 0 && Opaque(skin->border_color)) {
      // Inset by half the pen width so the stroke stays inside the panel.
      float hw = border_dev / 2.0f;
      Gdiplus::GraphicsPath path;
      AddRoundRect(path, static_cast<int>(hw), static_cast<int>(hw),
                   cx - border_dev, cy - border_dev, rc_dev);
      Gdiplus::Pen pen(ToGdiColor(skin->border_color), static_cast<float>(border_dev));
      g.DrawPath(&pen, &path);
    }
    g.ResetTransform();
    g.Flush(Gdiplus::FlushIntentionSync);
  }

  // ── 2. D2D/DWrite text onto a separate transparent DIB ──────────────────────
  if (!EnsureTextSurface(res, hdc, fcx, fcy))
    return FAMO_UI_E_PAINT_FAILED;
  HDC tdc = res->text_dc;
  void* tbits = res->text_bits;

  int32_t rv = FAMO_UI_OK;
  {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);  // 96 DPI → DIP == device px (layout already scaled by dpi)
    HRESULT hr = S_OK;
    if (!res->text_target) {
      hr = res->d2d->CreateDCRenderTarget(&props, &res->text_target);
      if (SUCCEEDED(hr)) FAMO_BENCHMARK_COUNT(d2d_target);
    }
    if (SUCCEEDED(hr) && !res->text_brush) {
      hr = res->text_target->CreateSolidColorBrush(D2D1::ColorF(0, 0),
                                                   &res->text_brush);
      if (SUCCEEDED(hr)) FAMO_BENCHMARK_COUNT(brush);
    }
    RECT full = {0, 0, fcx, fcy};
    if (SUCCEEDED(hr)) hr = res->text_target->BindDC(tdc, &full);
    if (SUCCEEDED(hr)) {
      ID2D1DCRenderTarget* rt = res->text_target.Get();
      ID2D1SolidColorBrush* brush = res->text_brush.Get();
      rt->BeginDraw();
      rt->Clear(D2D1::ColorF(0, 0.0f));  // transparent — only glyphs get alpha

      IDWriteTextFormat* fLabel = res->fmt[0].Get();
      IDWriteTextFormat* fText = res->fmt[1].Get();
      IDWriteTextFormat* fComment = res->fmt[2].Get();

      // Preedit stays visually quiet: one text run, a dotted ink rule under the
      // active segment, and a caret. It is only visible when inline_preedit is
      // off; otherwise the host app owns the marked-text presentation.
      if (Opaque(skin->text_color) && view->preedit.data && view->preedit.length_bytes) {
        std::wstring pw = Widen(view->preedit.data, view->preedit.length_bytes);
        uint32_t ss = layout->preedit_sel_start_wchar;
        uint32_t se = layout->preedit_sel_end_wchar;
        if (se > pw.size()) se = static_cast<uint32_t>(pw.size());
        if (ss > se) ss = se;
        const FamoRect& pr = layout->preedit;
        // The converting segment is marked with a soft tint block BEHIND the
        // glyphs, not a rule under them. Drawn before the run so the text sits on
        // top; the run itself is one colour, so no glyph splitting is needed.
        //
        // This replaces a dotted ink rule (TSF ATTR_INPUT / TF_LS_DOT). That rule
        // had to disable antialiasing to keep its dots from smearing, which at
        // high DPI reads as a row of grit, and it now stacked as a second
        // horizontal line 6px above the band separator. A block is also the same
        // shape language as the selection pill below it, so the two read as one
        // hierarchy.
        //
        // 10% is deliberate. An earlier SOLID accent block behind the preedit was
        // removed because a third saturated use of the accent in one panel read as
        // a spell-check error; at a tenth of that it is a wash, and the panel's
        // saturated accent stays reserved for the pill and the caret — which
        // check_soft_cursor still asserts pixel-wise.
        if (!vertical && se > ss && !RectEmpty2(pr)) {
          const int32_t before = MeasureW(res->dwrite.Get(), fText, pw.c_str(), ss);
          const int32_t active =
              MeasureW(res->dwrite.Get(), fText, pw.c_str() + ss, se - ss);
          if (active > 0) {
            rt->SetTransform(D2D1::Matrix3x2F::Translation(sm_f, sm_f));
            const float pad_x = static_cast<float>(Scale(3, dpi));
            const float pad_y = static_cast<float>(Scale(2, dpi));
            const float radius = static_cast<float>(Scale(5, dpi));
            D2D1_COLOR_F tint = ToColorF(Opaque(skin->hilited_back_color)
                                             ? skin->hilited_back_color
                                             : skin->text_color);
            tint.a *= 0.10f;
            brush->SetColor(tint);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        static_cast<float>(pr.left + before) - pad_x,
                        static_cast<float>(pr.top) - pad_y,
                        static_cast<float>(pr.left + before + active) + pad_x,
                        static_cast<float>(pr.bottom) + pad_y),
                    radius, radius),
                brush);
          }
        }
        brush->SetColor(ToColorF(skin->text_color));
        DrawRun(rt, fText, brush, pw.c_str(),
                static_cast<uint32_t>(pw.size()), pr, vertical, sm_f, sm_f);
        if (!vertical && !RectEmpty2(pr)) {
          rt->SetTransform(D2D1::Matrix3x2F::Translation(sm_f, sm_f));
          const uint32_t accent = Opaque(skin->hilited_back_color)
                                      ? skin->hilited_back_color
                                      : skin->text_color;
          uint32_t cursor = layout->preedit_cursor_wchar;
          if (cursor > pw.size()) cursor = static_cast<uint32_t>(pw.size());
          int32_t x = pr.left + MeasureW(res->dwrite.Get(), fText, pw.c_str(), cursor);
          if (x < pr.left) x = pr.left;
          // The caret sits AFTER the glyph it follows, so x == pr.right is the
          // legal end-of-preedit position. Don't pull it back inside the text
          // rect: at end-of-preedit that would leave no room and collapse the
          // caret back to 1px. It lands in the panel's margin_x, not clipped.
          if (x > pr.right) x = pr.right;
          // Width comes from the skin, which the host fills from
          // SPI_GETCARETWIDTH (user setting, 1..20). Guard on skin->size so an
          // older caller's smaller FamoSkin still paints (falls back to 2).
          const int32_t caret_logical =
              (skin->size >= offsetof(FamoSkin, caret_width) + sizeof(int32_t) &&
               skin->caret_width > 0)
                  ? skin->caret_width
                  : 2;
          const int32_t caret_width = (std::max)(1, Scale(caret_logical, dpi));
          // Rounded caps (radius = half width) → a capsule, not a bar. At 1px
          // this degenerates to the old sharp rect, which is correct.
          const float caret_r = caret_width * 0.5f;
          brush->SetColor(ToColorF(accent));
          rt->FillRoundedRectangle(
              D2D1::RoundedRect(
                  D2D1::RectF(static_cast<float>(x), static_cast<float>(pr.top),
                              static_cast<float>(x + caret_width),
                              static_cast<float>(pr.bottom)),
                  caret_r, caret_r),
              brush);
          rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
      }
      // aux / tips (comment font, primary text color)
      if (Opaque(skin->text_color)) {
        brush->SetColor(ToColorF(skin->text_color));
        DrawUtf8(rt, fComment, brush, input->aux, layout->aux, vertical, sm_f, sm_f);
      }
      // candidates
      const uint32_t n = layout->candidate_count;
      for (uint32_t i = 0; i < n && view->candidates; ++i) {
        FamoCandidate cand{};
        if (!famo_candidate_ui::ReadCandidate(
                view->candidates, view->candidate_count, i, &cand) ||
            !famo_candidate_ui::ReadableCandidate(cand))
          return FAMO_UI_E_INVALID_ARGUMENT;
        const FamoCandidateRects& r = layout->candidates[i];
        const bool hl = (i == view->highlighted_index);
        if (!RectEmpty2(r.label) && Opaque(skin->label_color)) {
          const uint32_t lc = hl && Opaque(skin->hilited_text_color)
                                  ? skin->hilited_text_color
                                  : skin->label_color;
          brush->SetColor(ToColorF(lc));
          if (vertical_list) {
            std::wstring label = Widen(cand.label.data, cand.label.length_bytes);
            label.push_back(L'.');
            DrawRun(rt, fText, brush, label.c_str(),
                    static_cast<uint32_t>(label.size()), r.label, vertical,
                    sm_f, sm_f);
          } else {
            DrawUtf8(rt, fLabel, brush, cand.label, r.label, vertical, sm_f,
                     sm_f);
          }
        }
        uint32_t tc = hl ? skin->hilited_text_color : skin->candidate_text_color;
        if (Opaque(tc)) {
          brush->SetColor(ToColorF(tc));
          DrawUtf8(rt, fText, brush, cand.text, r.text, vertical, sm_f, sm_f);
        }
        if (r.has_comment && !RectEmpty2(r.comment)) {
          uint32_t cc = hl ? skin->hilited_comment_color : skin->comment_color;
          if (Opaque(cc)) {
            brush->SetColor(ToColorF(cc));
            DrawUtf8(rt, fComment, brush, cand.comment, r.comment, vertical, sm_f, sm_f);
          }
        }
      }
      if (input->size >= offsetof(FamoLayoutInput, preview_page_size) +
                             sizeof(input->preview_page_size) &&
          input->preview_candidates) {
        const uint32_t preview_count = (std::min)(
            layout->preview_candidate_count,
            static_cast<uint32_t>(FAMO_MAX_PREVIEW_CANDIDATES));
        for (uint32_t i = 0; i < preview_count; ++i) {
          FamoCandidate candidate{};
          if (!famo_candidate_ui::ReadCandidate(
                  input->preview_candidates, input->preview_candidate_count, i,
                  &candidate) ||
              !famo_candidate_ui::ReadableCandidate(candidate))
            return FAMO_UI_E_INVALID_ARGUMENT;
          D2D1_COLOR_F color = ToColorF(skin->candidate_text_color);
          color.a *= 0.75f;
          brush->SetColor(color);
          DrawUtf8(rt, fText, brush, candidate.text,
                   layout->preview_candidates[i].text, false, sm_f, sm_f);
        }
      }
      // Page affordances use typographic chevrons rather than raw ASCII operators.
      if (!RectEmpty2(layout->prev_page) && Opaque(skin->prevpage_color)) {
        brush->SetColor(ToColorF(skin->prevpage_color));
        DrawRun(rt, fText, brush, L"‹", 1, layout->prev_page, vertical, sm_f, sm_f);
      }
      if (!RectEmpty2(layout->next_page) && Opaque(skin->nextpage_color)) {
        brush->SetColor(ToColorF(skin->nextpage_color));
        DrawRun(rt, fText, brush, L"›", 1, layout->next_page, vertical, sm_f, sm_f);
      }
      // ponytail: status-icon slot left un-drawn — the icon (中/英/全/半) comes from
      // an HICON the host owns, not from the neutral view; host paints it. Skin
      // status_icon_size==0 by default reserves no slot.
      hr = rt->EndDraw();
    }
    if (FAILED(hr)) {
      res->text_brush.Reset();
      res->text_target.Reset();
      rv = FAMO_UI_E_PAINT_FAILED;
    }
  }

  // EndDraw's blit to the DC may be in the GDI batch — flush before reading tbits.
  GdiFlush();

  // ── 3. Composite the text DIB over the shapes (source-over) ─────────────────
  if (rv == FAMO_UI_OK) {
    CompositePremultiplied(static_cast<uint32_t*>(ds.dsBm.bmBits),
                           ds.dsBm.bmWidthBytes / sizeof(uint32_t),
                           static_cast<const uint32_t*>(tbits), fcx, fcy);
  }

  return rv;
} catch (...) {
  return FAMO_UI_E_PAINT_FAILED;  // no C++ exception escapes the paint (R6)
}

// ── the status bar paint ─────────────────────────────────────────────────────
// Unlike PaintImpl this is a single D2D pass bound straight to the caller's DC:
// the bar has no drop shadow, so there is no GDI+ work to keep off the render
// target and nothing to composite afterwards. Shapes and text share one target.

// Centre one label in its button on its INKED extents. Alignment is set on the
// per-call layout, not on the shared IDWriteTextFormat — the candidate list
// draws through the same format and positions its runs itself, so it needs
// leading alignment intact.
//
// Centering the layout box is not enough: CJK punctuation is laid out in a
// full-width em box with the mark in one corner (。 sits low and left), so a
// box-centered 。 reads as a smudge next to three square glyphs. The overhang
// metrics give the real ink bounds relative to the layout box, and the residual
// between the two overhangs is exactly how far the ink sits off-centre. Square
// glyphs whose ink already fills the box get overhangs that cancel, so this
// correction is glyph-relative and leaves 中/繁/半 where they were.
void DrawCenteredLabel(FamoTextResources* res, ID2D1DCRenderTarget* rt,
                       ID2D1SolidColorBrush* brush, const FamoRect& rc,
                       const char* utf8) {
  IDWriteTextFormat* fmt = res->fmt[1].Get();
  if (!fmt || !utf8 || !*utf8 || RectEmpty2(rc)) return;
  std::wstring text = Widen(utf8, static_cast<uint32_t>(strlen(utf8)));
  if (text.empty()) return;
  const float w = static_cast<float>(rc.right - rc.left);
  const float h = static_cast<float>(rc.bottom - rc.top);
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(res->dwrite->CreateTextLayout(text.c_str(),
                                           static_cast<UINT32>(text.size()),
                                           fmt, w, h, &layout)) ||
      !layout)
    return;
  FAMO_BENCHMARK_COUNT(text_layout);
  layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  float x = static_cast<float>(rc.left);
  float y = static_cast<float>(rc.top);
  DWRITE_OVERHANG_METRICS overhang = {};
  if (SUCCEEDED(layout->GetOverhangMetrics(&overhang))) {
    // Overhangs are outward-positive, so ink centred in the box makes the two
    // sides cancel. Clamped: a decorative face can report a wild overhang, and
    // a glyph shoved outside its button is worse than one drawn off-centre.
    const float dx = (overhang.left - overhang.right) / 2.0f;
    const float dy = (overhang.top - overhang.bottom) / 2.0f;
    x += (std::max)(-w / 2.0f, (std::min)(w / 2.0f, dx));
    y += (std::max)(-h / 2.0f, (std::min)(h / 2.0f, dy));
  }
  rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush,
                     D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

D2D1_COLOR_F WithAlpha(uint32_t argb, float alpha) {
  D2D1_COLOR_F color = ToColorF(argb);
  color.a = alpha;
  return color;
}

// Compatibility fallback for callers built before FamoSkin carried card2.
uint32_t TroughColor(uint32_t card) {
  const int channel[3] = {static_cast<int>((card >> 16) & 0xff),
                          static_cast<int>((card >> 8) & 0xff),
                          static_cast<int>(card & 0xff)};
  const bool lighten = channel[0] < 16 && channel[1] < 16 && channel[2] < 16;
  uint32_t result = card & 0xFF000000u;
  for (int i = 0; i < 3; ++i) {
    const int step = (std::max)(5, (channel[i] * 45 + 500) / 1000);
    const int value = lighten ? (std::min)(255, channel[i] + 6)
                              : (std::max)(0, channel[i] - step);
    result |= static_cast<uint32_t>(value) << (16 - i * 8);
  }
  return result;
}

// One segment of the strip: outer corners rounded, inner edges square so
// adjacent segments share them. D2D has no per-corner rounded rect, so round
// the whole cell and square the inner side back with a plain rect.
void FillSegment(ID2D1DCRenderTarget* rt, ID2D1Brush* brush,
                 const D2D1_RECT_F& rc, float radius, bool round_left,
                 bool round_right) {
  if (!round_left && !round_right) {
    rt->FillRectangle(rc, brush);
    return;
  }
  rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), brush);
  if (!round_left)
    rt->FillRectangle(D2D1::RectF(rc.left, rc.top, rc.left + radius, rc.bottom),
                      brush);
  if (!round_right)
    rt->FillRectangle(
        D2D1::RectF(rc.right - radius, rc.top, rc.right, rc.bottom), brush);
}

int32_t StatusBarPaintImpl(const FamoStatusBarSpec* spec, const FamoSkin* skin,
                           FamoTextResources* res, HDC hdc) try {
  const int32_t cx = spec->bar_size.cx;
  const int32_t cy = spec->bar_size.cy;
  if (cx <= 0 || cy <= 0) return FAMO_UI_E_INVALID_ARGUMENT;
  if (spec->button_count && !spec->buttons) return FAMO_UI_E_INVALID_ARGUMENT;

  HBITMAP hbm = static_cast<HBITMAP>(GetCurrentObject(hdc, OBJ_BITMAP));
  DIBSECTION ds = {};
  if (!hbm || GetObjectW(hbm, sizeof(ds), &ds) != sizeof(ds) ||
      ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
    return FAMO_UI_E_INVALID_ARGUMENT;  // needs a 32-bit top-down DIB section
  }
  if (ds.dsBm.bmWidth != cx || ds.dsBm.bmHeight != cy)
    return FAMO_UI_E_INVALID_ARGUMENT;
  const uint32_t dpi = spec->dpi ? spec->dpi : 96u;
  const int32_t rc_dev = Scale(skin->round_corner, dpi);
  const float panel_r = static_cast<float>(rc_dev);
  // The strip is inset from the trough by however far the host placed the first
  // segment; subtracting that keeps the segment corners concentric with the
  // trough's instead of guessing a second radius.
  const int32_t inset = spec->button_count ? spec->buttons[0].bounds.left : 0;
  const float segment_r = static_cast<float>((std::max)(0, rc_dev - inset));
  const float border = static_cast<float>(Scale(skin->border, dpi));

  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0f, 96.0f);  // 96 DPI → DIP == device px (metrics already scaled)
  HRESULT hr = S_OK;
  if (!res->text_target) {
    hr = res->d2d->CreateDCRenderTarget(&props, &res->text_target);
    if (SUCCEEDED(hr)) FAMO_BENCHMARK_COUNT(d2d_target);
  }
  if (SUCCEEDED(hr) && !res->text_brush) {
    hr = res->text_target->CreateSolidColorBrush(D2D1::ColorF(0, 0),
                                                 &res->text_brush);
    if (SUCCEEDED(hr)) FAMO_BENCHMARK_COUNT(brush);
  }
  RECT full = {0, 0, cx, cy};
  if (SUCCEEDED(hr)) hr = res->text_target->BindDC(hdc, &full);
  if (FAILED(hr)) {
    res->text_brush.Reset();
    res->text_target.Reset();
    return FAMO_UI_E_PAINT_FAILED;
  }

  ID2D1DCRenderTarget* rt = res->text_target.Get();
  ID2D1SolidColorBrush* brush = res->text_brush.Get();
  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0, 0.0f));  // transparent — round corners stay cut out

  // Trough first: the segments sit in it, so it is the bar's background.
  const D2D1_RECT_F panel =
      D2D1::RectF(0.0f, 0.0f, static_cast<float>(cx), static_cast<float>(cy));
  const bool has_card2 =
      skin->size >= offsetof(FamoSkin, card2_color) + sizeof(uint32_t) &&
      Opaque(skin->card2_color);
  if (Opaque(skin->back_color)) {
    brush->SetColor(ToColorF(has_card2 ? skin->card2_color
                                      : TroughColor(skin->back_color)));
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel, panel_r, panel_r), brush);
  }
  if (border > 0.0f && Opaque(skin->border_color)) {
    // Inset by half the stroke width so the border stays inside the bitmap.
    const D2D1_RECT_F edge = D2D1::RectF(border / 2.0f, border / 2.0f,
                                         static_cast<float>(cx) - border / 2.0f,
                                         static_cast<float>(cy) - border / 2.0f);
    brush->SetColor(ToColorF(skin->border_color));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(edge, panel_r, panel_r), brush,
                             border);
  }

  for (uint32_t i = 0; i < spec->button_count; ++i) {
    const FamoStatusBarButton& button = spec->buttons[i];
    if (RectEmpty2(button.bounds)) continue;
    const D2D1_RECT_F cell = D2D1::RectF(static_cast<float>(button.bounds.left),
                                         static_cast<float>(button.bounds.top),
                                         static_cast<float>(button.bounds.right),
                                         static_cast<float>(button.bounds.bottom));
    // Rounding follows adjacency, not position: a segment rounds an edge that
    // no neighbour touches. A run of bare trough between two groups therefore
    // rounds both facing edges on its own, with no extra field to pass in.
    const bool first =
        i == 0 || spec->buttons[i - 1].bounds.right != button.bounds.left;
    const bool last = i + 1 == spec->button_count ||
                      spec->buttons[i + 1].bounds.left != button.bounds.right;
    // The current state is already carried by the label (中/英, 简/繁, ...), so
    // clicking never needs the skin accent behind it.
    const uint32_t fill_color = skin->back_color;
    const uint32_t label_color = skin->text_color;
    if (Opaque(fill_color)) {
      brush->SetColor(ToColorF(fill_color));
      FillSegment(rt, brush, cell, segment_r, first, last);
    }
    if (!Opaque(label_color)) continue;
    if (button.pressed || button.hover) {
      brush->SetColor(WithAlpha(label_color, 0.10f));
      FillSegment(rt, brush, cell, segment_r, first, last);
    }
    brush->SetColor(ToColorF(label_color));
    DrawCenteredLabel(res, rt, brush, button.bounds, button.label);
    // Hairline between neighbours that actually touch; a gap needs no divider.
    if (!last && Opaque(skin->border_color)) {
      const float line = (std::max)(1.0f, border);
      const float trim = static_cast<float>(Scale(7, dpi));
      brush->SetColor(ToColorF(skin->border_color));
      rt->FillRectangle(D2D1::RectF(cell.right - line / 2.0f, cell.top + trim,
                                    cell.right + line / 2.0f,
                                    cell.bottom - trim),
                        brush);
    }
  }

  hr = rt->EndDraw();
  if (FAILED(hr)) {
    res->text_brush.Reset();
    res->text_target.Reset();
    return FAMO_UI_E_PAINT_FAILED;
  }
  // EndDraw's blit to the DC may still sit in the GDI batch when the caller
  // hands the same bitmap to UpdateLayeredWindow.
  GdiFlush();
  return FAMO_UI_OK;
} catch (...) {
  return FAMO_UI_E_PAINT_FAILED;  // no C++ exception escapes the paint (R6)
}

}  // namespace

// ── FamoTextResources lifecycle + measurement callback ───────────────────────
extern "C" FamoTextResources* FamoTextResourcesCreate(const FamoSkin* skin,
                                                       uint32_t dpi) {
  if (!SkinResourceInputsValid(skin)) return nullptr;
  FamoTextResources* res = new (std::nothrow) FamoTextResources();
  if (!res) return nullptr;

  Gdiplus::GdiplusStartupInput gsi;
  if (Gdiplus::GdiplusStartup(&res->gdiplus_token, &gsi, nullptr) != Gdiplus::Ok) {
    delete res;
    return nullptr;
  }
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               res->d2d.GetAddressOf())) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(res->dwrite.GetAddressOf())))) {
    ULONG_PTR tok = res->gdiplus_token;
    delete res;
    Gdiplus::GdiplusShutdown(tok);
    return nullptr;
  }
  FamoTextResourcesReconfigure(res, skin, dpi);
  return res;
}

extern "C" int32_t FamoTextResourcesReconfigure(FamoTextResources* res,
                                                  const FamoSkin* skin,
                                                  uint32_t dpi) {
  if (!res || !SkinResourceInputsValid(skin) || !res->dwrite)
    return FAMO_UI_E_INVALID_ARGUMENT;
  if (dpi == 0) dpi = 96;
  ComPtr<IDWriteTextFormat> formats[3];
  const FamoFontSpec* specs[3] = {&skin->label_font, &skin->text_font,
                                  &skin->comment_font};
  // A bad face remains non-fatal: that run measures 0 / draws nothing.
  for (int i = 0; i < 3; ++i)
    MakeFormat(res->dwrite.Get(), *specs[i], dpi, formats[i].GetAddressOf());
  for (int i = 0; i < 3; ++i) res->fmt[i] = std::move(formats[i]);
  res->shadow_pixels.clear();
  res->shadow_width = 0;
  res->shadow_height = 0;
  return FAMO_UI_OK;
}

extern "C" void FamoTextResourcesDiscardDeviceResources(
    FamoTextResources* res) {
  if (!res) return;
  res->text_brush.Reset();
  res->text_target.Reset();
}

extern "C" void FamoTextResourcesDestroy(FamoTextResources* res) {
  if (!res) return;
  ULONG_PTR tok = res->gdiplus_token;
  ReleaseTextSurface(res);
  delete res;  // releases the D2D/DWrite ComPtrs
  Gdiplus::GdiplusShutdown(tok);
}

extern "C" int32_t FamoTextMeasure(void* user, int32_t which, const char* utf8,
                                   uint32_t utf8_len) {
  FamoTextResources* res = static_cast<FamoTextResources*>(user);
  if (!res || which < 0 || which > 2 || !utf8 || utf8_len == 0) return 0;
  IDWriteTextFormat* fmt = res->fmt[which].Get();
  if (!fmt) return 0;
  std::wstring w = Widen(utf8, utf8_len);
  if (w.empty()) return 0;
  ComPtr<IDWriteTextLayout> tl;
  if (FAILED(res->dwrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()),
                                           fmt, 1e6f, 1e6f, &tl)))
    return 0;
  FAMO_BENCHMARK_COUNT(text_layout);
  DWRITE_TEXT_METRICS tm = {};
  if (FAILED(tl->GetMetrics(&tm))) return 0;
  return static_cast<int32_t>(tm.widthIncludingTrailingWhitespace + 0.5f);
}

#ifdef FAMO_CANDIDATE_UI_BENCHMARK_COUNTERS
extern "C" void FamoBenchmarkRenderCountersReset() {
  g_benchmark_counters = {};
}

extern "C" FamoBenchmarkRenderCounters FamoBenchmarkRenderCountersSnapshot() {
  return g_benchmark_counters;
}
#endif

// ── Paint entry point: SEH-wraps PaintImpl so a hard fault can't drop a key ────
extern "C" int32_t FamoCandidateUiPaint(const FamoCompositionView* view,
                                        const FamoSkin* skin,
                                        const FamoLayoutInput* input,
                                        const FamoLayoutResult* layout,
                                        FamoTextResources* res, void* mem_dc) {
  if (!view || !skin || !input || !layout || !res || !mem_dc)
    return FAMO_UI_E_INVALID_ARGUMENT;
  if (!CandidatePaintInputsValid(view, skin, input, layout))
    return FAMO_UI_E_INVALID_ARGUMENT;
  // This frame has no unwinding locals → legal to guard with SEH; PaintImpl holds
  // the C++ objects and catches C++ exceptions itself. __except only sees hard
  // faults (e.g. a bogus DC), so a paint crash degrades to a hidden popup, never
  // a dropped keystroke or corrupted session (design §6 crash isolation, R6).
  __try {
    return PaintImpl(view, skin, input, layout, res, static_cast<HDC>(mem_dc));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return FAMO_UI_E_PAINT_FAILED;
  }
}

// ── Status bar paint entry point: same SEH contract as the candidate paint ────
extern "C" int32_t FamoStatusBarPaint(const FamoStatusBarSpec* spec,
                                      const FamoSkin* skin,
                                      FamoTextResources* res, void* mem_dc) {
  if (!spec || !skin || !res || !mem_dc) return FAMO_UI_E_INVALID_ARGUMENT;
  if (spec->size < sizeof(FamoStatusBarSpec) ||
      skin->size < offsetof(FamoSkin, caret_width) ||
      (spec->button_count > 0 && !spec->buttons))
    return FAMO_UI_E_INVALID_ARGUMENT;
  __try {
    return StatusBarPaintImpl(spec, skin, res, static_cast<HDC>(mem_dc));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return FAMO_UI_E_PAINT_FAILED;
  }
}
