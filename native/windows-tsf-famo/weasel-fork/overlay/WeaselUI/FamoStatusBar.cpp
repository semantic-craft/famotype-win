#include "stdafx.h"

#include "FamoStatusBar.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <shellscalingapi.h>  // GetDpiForMonitor（Win8.1+，与 WeaselPanel 一致）

#include <cwchar>  // wcslen
#include <cmath>   // fabsf
#include <fstream>  // 位置持久化

#pragma comment(lib, "Shcore.lib")

// 阶段 1B（完整状态栏）：右下角圆角浮窗，一排开关按钮
//   中/Ａ(ascii_mode)  ，/.(ascii_punct)  简/繁(traditionalization + zh_trad)  半/全(full_shape)  ☰(展开)
// 点击开关 -> 翻转影子态 + 下发 SetOption + 重绘；点击 ☰ -> 触发 on_expand_（阶段 2 接面板）。
//
// 绘制路径严格照搬 WeaselPanel::DoPaint / _LayerUpdate：
//   离屏 memDC -> Direct2D BindDC/BeginDraw/EndDraw -> UpdateLayeredWindow(ULW_ALPHA)。

namespace {
// 0xRRGGBB -> D2D1::ColorF（rgb, alpha）。
inline D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.0f) {
  return D2D1::ColorF(rgb, a);
}
// Rime/Weasel 颜色 0xAABBGGRR（COLORREF + 高字节 alpha）-> D2D1::ColorF。
// 与 WeaselPanel::_TextOut 同算法（R=低字节，alpha=高字节）。
inline D2D1_COLOR_F FromRimeColor(int c) {
  const float a = ((c >> 24) & 0xff) / 255.0f;
  const float r = (c & 0xff) / 255.0f;
  const float g = ((c >> 8) & 0xff) / 255.0f;
  const float b = ((c >> 16) & 0xff) / 255.0f;
  return D2D1::ColorF(r, g, b, a);
}
// 取色：alpha 非 0 用皮肤色，否则回退。
inline D2D1_COLOR_F PickColor(int c, D2D1_COLOR_F fallback) {
  return ((c >> 24) & 0xff) ? FromRimeColor(c) : fallback;
}
inline float Lum(const D2D1_COLOR_F& c) {
  return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}
// 保证文字与背景对比：强制不透明；若亮度差不足则转黑/白（修复关态在某些皮肤下偏淡）。
inline D2D1_COLOR_F Legible(D2D1_COLOR_F text, const D2D1_COLOR_F& bg) {
  text.a = 1.0f;
  if (fabsf(Lum(text) - Lum(bg)) < 0.4f)
    return (Lum(bg) > 0.5f) ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)
                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
  return text;
}
inline float Clamp01(float v) {
  if (v < 0.0f)
    return 0.0f;
  if (v > 1.0f)
    return 1.0f;
  return v;
}
inline D2D1_COLOR_F ScaleColor(D2D1_COLOR_F c, float factor) {
  return D2D1::ColorF(Clamp01(c.r * factor), Clamp01(c.g * factor),
                      Clamp01(c.b * factor), c.a);
}
inline D2D1_COLOR_F InteractionFill(D2D1_COLOR_F base, bool press) {
  const bool light = Lum(base) > 0.5f;
  const float factor = press ? (light ? 0.82f : 1.34f)
                             : (light ? 0.92f : 1.18f);
  return ScaleColor(base, factor);
}
inline FamoStatusBarPoint ClientPoint(LPARAM lParam) {
  return {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}
inline bool MatchesOption(const std::string& button_option,
                          const std::string& requested_option) {
  if (button_option == requested_option)
    return true;
  return button_option == "traditionalization" && requested_option == "zh_trad";
}
}  // namespace

FamoStatusBar::FamoStatusBar() {
  // DWR 仅用于绘制按钮文字，给一套最小字体配置即可（字体回退保证可渲染）。
  style_.font_face = L"Microsoft YaHei UI";
  style_.label_font_face = style_.font_face;
  style_.comment_font_face = style_.font_face;
  style_.font_point = 14;
  style_.label_font_point = 14;
  style_.comment_font_point = 14;
  _BuildButtons();
}

FamoStatusBar::~FamoStatusBar() {}

void FamoStatusBar::_BuildButtons() {
  buttons_.clear();
  // 标签与 famo schema switches 对齐：ascii_mode[中,Ａ]、ascii_punct[¥,$]、
  // traditionalization/zh_trad[简,繁]、full_shape[半角,全角]。状态栏用单字形更紧凑。
  buttons_.push_back({{}, "ascii_mode", &opt_ascii_mode_, L"中", L"Ａ"});
  buttons_.push_back({{}, "ascii_punct", &opt_ascii_punct_, L"，", L"."});
  buttons_.push_back(
      {{}, "traditionalization", &opt_traditionalization_, L"简", L"繁"});
  buttons_.push_back({{}, "full_shape", &opt_full_shape_, L"半", L"全"});
  buttons_.push_back({{}, "", nullptr, L"☰", L"☰", ButtonAction::Expand});
}

UINT FamoStatusBar::_CurrentDpi() const {
  HMONITOR mon = m_hWnd
                     ? ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY)
                     : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  UINT dx = 96, dy = 96;
  if (SUCCEEDED(::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx)
    return dx;
  return 96;
}

int FamoStatusBar::_BarWidthPx() const {
  const int n = static_cast<int>(buttons_.size());
  return _Scale(kPad) * 2 + n * _Scale(kCell) +
         (n > 0 ? (n - 1) * _Scale(kGap) : 0);
}

int FamoStatusBar::_BarHeightPx() const {
  return _Scale(bar_h_logical_);
}

void FamoStatusBar::_EnsureResources() {
  if (dwr_)
    return;
  dwr_ = std::make_shared<weasel::DirectWriteResources>(style_, dpi_);
  dwr_->InitResources(style_, dpi_);
  if (dwr_->pTextFormat) {
    dwr_->pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dwr_->pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
}

void FamoStatusBar::_LayoutButtons() {
  const int cell = _Scale(kCell);
  const int gap = _Scale(kGap);
  const int pad = _Scale(kPad);
  const int top = (_BarHeightPx() - cell) / 2;
  int left = pad;
  std::vector<FamoStatusBarRect> rects;
  rects.reserve(buttons_.size());
  for (auto& b : buttons_) {
    b.rc = {left, top, left + cell, top + cell};
    rects.push_back({b.rc.left, b.rc.top, b.rc.right, b.rc.bottom});
    left += cell + gap;
  }
  interaction_.SetButtonRects(std::move(rects));
}

bool FamoStatusBar::CreateBar(HWND owner) {
  dpi_ = _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  _LayoutButtons();
  RECT init = {0, 0, _BarWidthPx(), _BarHeightPx()};
  // dwStyle/dwExStyle = 0 -> 采用 FamoStatusBarTraits。
  HWND hwnd = Create(owner, &init, NULL, 0, 0, 0U, NULL);
  if (hwnd)
    _LoadPos();  // 有持久化位则恢复（置 m_user_moved）
  return hwnd != NULL;
}

void FamoStatusBar::Reposition() {
  if (!m_hWnd)
    return;
  HMONITOR mon = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(MONITORINFO)};
  if (!::GetMonitorInfo(mon, &mi))
    return;
  dpi_ = _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  _LayoutButtons();
  const int w = _BarWidthPx();
  const int h = _BarHeightPx();
  int x, y;
  if (m_user_moved) {
    // 用户拖过：保持当前位置（仅随 DPI 更新尺寸），不回右下角。
    RECT wr = {0, 0, 0, 0};
    ::GetWindowRect(m_hWnd, &wr);
    x = wr.left;
    y = wr.top;
  } else {
    const int margin = _Scale(12);
    x = mi.rcWork.right - w - margin;
    y = mi.rcWork.bottom - h - margin;
  }
  SetWindowPos(HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void FamoStatusBar::ShowBar() {
  if (!m_hWnd)
    return;
  Reposition();
  Refresh();
  ShowWindow(SW_SHOWNOACTIVATE);
}

void FamoStatusBar::ShowOnFocus() {
  if (!m_hWnd)
    return;
  dpi_ = _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  _LayoutButtons();
  const int w = _BarWidthPx();
  const int h = _BarHeightPx();
  int x, y;
  if (m_user_moved) {
    // 拖过/有持久化位：保持当前位置（仅夹回可见区，防显示器变更后跑屏外）。
    RECT wr = {0, 0, 0, 0};
    ::GetWindowRect(m_hWnd, &wr);
    x = wr.left;
    y = wr.top;
  } else {
    // 固定定位：显示器工作区右下角（不跟随前台窗口，避免切窗口时状态栏乱跳）。
    HMONITOR mon = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    ::GetMonitorInfo(mon, &mi);
    const int margin = _Scale(12);
    x = mi.rcWork.right - w - margin;
    y = mi.rcWork.bottom - h - margin;
  }
  _ClampToMonitor(x, y, w, h);
  SetWindowPos(HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  Refresh();
  ShowWindow(SW_SHOWNOACTIVATE);
}

void FamoStatusBar::SetStatePath(const std::wstring& path) {
  state_path_ = path;
}

void FamoStatusBar::_DrawExpandGlyph(const RECT& button_rc,
                                     bool hover,
                                     bool press) {
  if (!dwr_ || !dwr_->pRenderTarget)
    return;

  auto& rt = dwr_->pRenderTarget;
  // 三圆点「更多」：与整栏设计语言同构——半径复用开态指示点(_Scale(2))，
  // 静止态用关态文字灰，hover/press 才染品牌酒红（酒红只在状态/交互时出现）。
  const float cx = ((float)button_rc.left + (float)button_rc.right) * 0.5f;
  const float cy = ((float)button_rc.top + (float)button_rc.bottom) * 0.5f;
  const float r = static_cast<float>(_Scale(2));
  const float sp = 5.5f * dpi_scale_;  // 圆心间距（逻辑 5.5，浮点缩放免累积误差）
  dwr_->CreateBrush((hover || press) ? c_hi_bg_ : c_text_);
  for (int i = -1; i <= 1; ++i) {
    D2D1_ELLIPSE dot = {D2D1::Point2F(cx + sp * i, cy), r, r};
    rt->FillEllipse(dot, dwr_->pBrush.Get());
  }
}

void FamoStatusBar::_ClampToMonitor(int& x, int& y, int w, int h) const {
  POINT c = {x + w / 2, y + h / 2};
  HMONITOR mon = ::MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(MONITORINFO)};
  if (!::GetMonitorInfo(mon, &mi))
    return;
  if (x + w > mi.rcWork.right)
    x = mi.rcWork.right - w;
  if (y + h > mi.rcWork.bottom)
    y = mi.rcWork.bottom - h;
  if (x < mi.rcWork.left)
    x = mi.rcWork.left;
  if (y < mi.rcWork.top)
    y = mi.rcWork.top;
}

void FamoStatusBar::_SavePos() {
  if (state_path_.empty())
    return;
  RECT wr = {0, 0, 0, 0};
  if (!::GetWindowRect(m_hWnd, &wr))
    return;
  std::ofstream f(state_path_, std::ios::trunc);
  if (f)
    f << wr.left << ' ' << wr.top << '\n';  // 物理屏幕坐标
}

void FamoStatusBar::_LoadPos() {
  if (state_path_.empty())
    return;
  std::ifstream f(state_path_);
  int x = 0, y = 0;
  if (f && (f >> x >> y)) {
    m_user_moved = true;
    int w = _BarWidthPx(), h = _BarHeightPx();
    _ClampToMonitor(x, y, w, h);
    SetWindowPos(NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
  }
}

void FamoStatusBar::HideBar() {
  if (m_hWnd)
    ShowWindow(SW_HIDE);
}

void FamoStatusBar::Refresh() {
  if (m_hWnd)
    DoPaint();
}

void FamoStatusBar::SyncStatus(bool ascii_mode, bool full_shape) {
  if (opt_ascii_mode_ == ascii_mode && opt_full_shape_ == full_shape)
    return;  // 无变化，避免重绘风暴
  opt_ascii_mode_ = ascii_mode;
  opt_full_shape_ = full_shape;
  Refresh();
}

void FamoStatusBar::ApplySkin(const weasel::UIStyle& s) {
  // 变更检测：背景/候选文字/字体名 三键不变则跳过（避免每次 OnUpdateUI 重建）。
  if (skin_applied_ && s.back_color == skin_key_back_ &&
      s.text_color == skin_key_text_ && s.font_face == skin_key_font_)
    return;
  skin_applied_ = true;
  skin_key_back_ = s.back_color;
  skin_key_text_ = s.text_color;
  skin_key_font_ = s.font_face;

  // 颜色：候选优先，回退普通，再回退深色默认。
  c_bg_ = PickColor(s.back_color, Rgb(0x2B2B2B, 0.94f));
  // 关态标签用「普通文字色」（比候选文字更淡），让静止态更克制；回退候选色、再回退灰。
  c_text_ = PickColor(s.text_color ? s.text_color : s.candidate_text_color,
                      Rgb(0xCCCCCC, 1.0f));
  c_hi_bg_ = PickColor(s.hilited_candidate_back_color ? s.hilited_candidate_back_color
                                                      : s.hilited_back_color,
                       Rgb(0x3B6EA5, 0.95f));
  c_hi_text_ = PickColor(s.hilited_candidate_text_color ? s.hilited_candidate_text_color
                                                        : s.hilited_text_color,
                         Rgb(0xFFFFFF, 1.0f));
  // 关态/高亮态文字都保证与各自背景足够对比（修复深色皮肤下关态偏淡）。
  c_text_ = Legible(c_text_, c_bg_);
  c_hi_text_ = Legible(c_hi_text_, c_hi_bg_);
  round_corner_logical_ = s.round_corner > 0 ? s.round_corner : 8;

  // 字体：用皮肤字族保持一致；字号保持状态栏紧凑值（候选字号过大不适合小格）。
  if (!s.font_face.empty()) {
    style_.font_face = s.font_face;
    style_.label_font_face = s.font_face;
    style_.comment_font_face = s.font_face;
    dwr_.reset();  // 字体变 -> 重建 DWR
  }
  Refresh();
}

void FamoStatusBar::ToggleOption(const std::string& opt) {
  for (auto& b : buttons_) {
    if (MatchesOption(b.option, opt) && b.state) {
      *b.state = !*b.state;
      if (setter_) {
        setter_(b.option, *b.state);
        if (b.option == "traditionalization")
          setter_("zh_trad", *b.state);
      }
      Refresh();
      return;
    }
  }
}

bool FamoStatusBar::QueryOption(const std::string& opt) const {
  for (const auto& b : buttons_) {
    if (MatchesOption(b.option, opt) && b.state)
      return *b.state;
  }
  return false;
}

POINT FamoStatusBar::AnchorTopLeftScreen() const {
  RECT wr = {0, 0, 0, 0};
  if (m_hWnd)
    ::GetWindowRect(m_hWnd, &wr);
  POINT p = {wr.left, wr.top};
  return p;
}

void FamoStatusBar::DoPaint() {
  // 全程保持 WS_EX_LAYERED（搜狗式：UpdateLayeredWindow 为唯一绘制路径）。
  ModifyStyleEx(WS_EX_TRANSPARENT, WS_EX_LAYERED);

  CRect rc;
  GetClientRect(&rc);
  if (rc.Width() <= 0 || rc.Height() <= 0)
    return;
  if (buttons_.empty())
    _BuildButtons();

  HDC hdc = ::GetDC(m_hWnd);
  if (!hdc)
    return;
  HDC memDC = ::CreateCompatibleDC(hdc);
  HBITMAP memBmp = memDC ? ::CreateCompatibleBitmap(hdc, rc.Width(), rc.Height()) : NULL;
  ::ReleaseDC(m_hWnd, hdc);
  if (!memDC || !memBmp) {
    if (memDC)
      ::DeleteDC(memDC);
    if (memBmp)
      ::DeleteObject(memBmp);
    return;
  }
  HGDIOBJ oldBmp = ::SelectObject(memDC, memBmp);
  if (!oldBmp) {
    ::DeleteObject(memBmp);
    ::DeleteDC(memDC);
    return;
  }

  _EnsureResources();
  if (!dwr_ || !dwr_->pRenderTarget) {
    ::SelectObject(memDC, oldBmp);
    ::DeleteObject(memBmp);
    ::DeleteDC(memDC);
    return;
  }

  auto rt = dwr_->pRenderTarget;
  HRESULT hr = rt->BindDC(memDC, &rc);
  if (FAILED(hr)) {
    dwr_.reset();
    _EnsureResources();
    if (!dwr_ || !dwr_->pRenderTarget) {
      ::SelectObject(memDC, oldBmp);
      ::DeleteObject(memBmp);
      ::DeleteDC(memDC);
      return;
    }
    rt = dwr_->pRenderTarget;
    hr = rt->BindDC(memDC, &rc);
  }
  if (FAILED(hr)) {
    dwr_.reset();
    ::SelectObject(memDC, oldBmp);
    ::DeleteObject(memBmp);
    ::DeleteDC(memDC);
    return;
  }

  rt->BeginDraw();
  rt->Clear(Rgb(0x000000, 0.0f));  // 全透明背景

  // 圆角背景（皮肤色，与候选窗一致）。
  const float radius = static_cast<float>(_Scale(round_corner_logical_));
  D2D1_ROUNDED_RECT bg = {D2D1::RectF(0.5f, 0.5f, (float)rc.Width() - 0.5f,
                                      (float)rc.Height() - 0.5f),
                          radius, radius};
  dwr_->CreateBrush(c_bg_);
  rt->FillRoundedRectangle(bg, dwr_->pBrush.Get());

  // 逐按钮绘制。开关「开」态：皮肤高亮底 + 高亮文字；「关」态/动作键：皮肤普通文字。
  const float cellRound = static_cast<float>(_Scale(6));
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const auto& b = buttons_[i];
    const bool on = (b.state && *b.state);
    const bool hover = (interaction_.HoverIndex() == static_cast<int>(i));
    const bool press = (interaction_.PressIndex() == static_cast<int>(i)) &&
                       !interaction_.PressOutside();
    const wchar_t* label = (b.state && *b.state) ? b.label1 : b.label0;

    // 克制描边方案：开态不再铺色块，仅 hover/press 时给一层轻底以示可点。
    if (hover || press) {
      D2D1_COLOR_F fill = InteractionFill(c_bg_, press);
      D2D1_ROUNDED_RECT hl = {
          D2D1::RectF((float)b.rc.left, (float)b.rc.top, (float)b.rc.right,
                      (float)b.rc.bottom),
          cellRound, cellRound};
      dwr_->CreateBrush(fill);
      rt->FillRoundedRectangle(hl, dwr_->pBrush.Get());
    }

    // 展开按钮：三圆点「更多」（静止灰，hover/press 酒红）。
    if (b.action == ButtonAction::Expand) {
      _DrawExpandGlyph(b.rc, hover, press);
      continue;
    }

    // 开态：品牌酒红文字（半粗）+ 底部小圆点；关态：普通文字色。
    dwr_->SetBrushColor(on ? c_hi_bg_ : c_text_);
    if (dwr_->pTextFormat && label && *label) {
      const UINT32 len = static_cast<UINT32>(wcslen(label));
      const float cw = static_cast<float>(b.rc.right - b.rc.left);
      const float ch = static_cast<float>(b.rc.bottom - b.rc.top);
      dwr_->CreateTextLayout(label, len, dwr_->pTextFormat.Get(), cw, ch);
      if (on && dwr_->pTextLayout) {
        DWRITE_TEXT_RANGE all = {0, len};
        dwr_->pTextLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
      }
      dwr_->DrawTextLayoutAt(
          D2D1::Point2F((float)b.rc.left, (float)b.rc.top));
      dwr_->ResetLayout();
    }

    // 开态指示点：候选高亮色小圆点，居中于按钮底部。
    if (on) {
      const float dot_cx = ((float)b.rc.left + (float)b.rc.right) * 0.5f;
      const float dot_cy = (float)b.rc.bottom - static_cast<float>(_Scale(5));
      const float dot_r = static_cast<float>(_Scale(2));
      D2D1_ELLIPSE dot = {D2D1::Point2F(dot_cx, dot_cy), dot_r, dot_r};
      dwr_->CreateBrush(c_hi_bg_);
      rt->FillEllipse(dot, dwr_->pBrush.Get());
    }
  }

  hr = rt->EndDraw();
  if (FAILED(hr)) {
    dwr_.reset();
    ::SelectObject(memDC, oldBmp);
    ::DeleteObject(memBmp);
    ::DeleteDC(memDC);
    return;
  }
  _LayerUpdate(rc, memDC);

  ::SelectObject(memDC, oldBmp);
  ::DeleteObject(memBmp);
  ::DeleteDC(memDC);
}

void FamoStatusBar::_LayerUpdate(const RECT& rc, HDC dc) {
  if (!m_hWnd || !dc || rc.right <= rc.left || rc.bottom <= rc.top)
    return;
  HDC screen = ::GetDC(NULL);
  if (!screen)
    return;
  CRect wr;
  GetWindowRect(&wr);
  POINT pos = {wr.left, wr.top};
  POINT src = {0, 0};
  SIZE sz = {rc.right - rc.left, rc.bottom - rc.top};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0xFF, AC_SRC_ALPHA};
  ::UpdateLayeredWindow(m_hWnd, screen, &pos, &sz, dc, &src, RGB(0, 0, 0), &bf,
                        ULW_ALPHA);
  ::ReleaseDC(NULL, screen);
}

LRESULT FamoStatusBar::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  _LayoutButtons();
  return 0;
}

LRESULT FamoStatusBar::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
  dwr_.reset();
  return 0;
}

LRESULT FamoStatusBar::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL& bHandled) {
  bHandled = TRUE;
  return MA_NOACTIVATE;  // 绝不抢焦点：点击状态栏不激活本窗口
}

LRESULT FamoStatusBar::OnNcHitTest(UINT, WPARAM, LPARAM, BOOL& bHandled) {
  bHandled = TRUE;
  return HTCLIENT;  // 整个悬浮状态条矩形都接收鼠标消息，避免 layered hit-test 漏点
}

LRESULT FamoStatusBar::OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&) {
  ::GetCursorPos(&m_drag_start);
  RECT wr = {0, 0, 0, 0};
  ::GetWindowRect(m_hWnd, &wr);
  m_win_start = {wr.left, wr.top};
  m_maybe_drag = true;
  m_dragging = false;
  interaction_.LeftDown(ClientPoint(lParam));
  ::SetCapture(m_hWnd);
  if (interaction_.PressIndex() >= 0)
    Refresh();
  return 0;
}

LRESULT FamoStatusBar::OnMouseMove(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
  const bool changed = interaction_.MouseMove(ClientPoint(lParam));
  if (interaction_.HoverIndex() >= 0 || interaction_.PressIndex() >= 0) {
    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT)};
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = m_hWnd;
    ::TrackMouseEvent(&tme);
  }
  if (changed)
    Refresh();
  if (!m_maybe_drag || (wParam & MK_LBUTTON) == 0)
    return 0;
  POINT cur;
  ::GetCursorPos(&cur);
  const int dx = cur.x - m_drag_start.x;
  const int dy = cur.y - m_drag_start.y;
  if (!m_dragging && (abs(dx) + abs(dy) > _Scale(4)))
    m_dragging = true;  // 超阈值 -> 判定为拖拽
  if (m_dragging)
    SetWindowPos(HWND_TOPMOST, m_win_start.x + dx, m_win_start.y + dy, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
  return 0;
}

LRESULT FamoStatusBar::OnLButtonUp(UINT, WPARAM, LPARAM lParam, BOOL&) {
  const bool was_drag = m_dragging;
  FamoStatusBarClick click = interaction_.LeftUp(ClientPoint(lParam), was_drag);
  if (::GetCapture() == m_hWnd)
    ::ReleaseCapture();
  m_maybe_drag = false;
  m_dragging = false;
  if (was_drag) {
    m_user_moved = true;  // 拖动过 -> 不再自动回右下角；本次抬起不算点击
    _SavePos();           // 持久化新位置（跨会话记住）
    Refresh();
    return 0;
  }
  // 未拖动 = 普通点击。只有按下和抬起都在同一按钮内，且中途未拖出，才触发。
  if (click.has_value && click.index >= 0 &&
      click.index < static_cast<int>(buttons_.size())) {
    const auto& b = buttons_[click.index];
    if (!b.option.empty() && b.state) {
      ToggleOption(b.option);  // 直达 RimeWithWeaselHandler::SetOption
      return 0;
    } else if (b.action == ButtonAction::Expand && on_expand_) {
      on_expand_();  // 展开按钮（阶段 2 接弹出面板）
    }
    Refresh();  // 图标随状态变化，同时清理 press 视觉态
    return 0;
  }
  Refresh();  // 清理 press 视觉态
  return 0;
}

LRESULT FamoStatusBar::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&) {
  if (interaction_.MouseLeave())
    Refresh();
  return 0;
}

LRESULT FamoStatusBar::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&) {
  const bool changed = interaction_.CaptureChanged() || m_maybe_drag || m_dragging;
  m_maybe_drag = false;
  m_dragging = false;
  if (changed)
    Refresh();
  return 0;
}

LRESULT FamoStatusBar::OnRButtonDown(UINT, WPARAM, LPARAM, BOOL& bHandled) {
  bHandled = TRUE;
  return 0;
}

LRESULT FamoStatusBar::OnRButtonUp(UINT, WPARAM, LPARAM, BOOL&) {
  if (on_right_click_)
    on_right_click_(AnchorTopLeftScreen());
  return 0;
}

LRESULT FamoStatusBar::OnDpiChanged(UINT, WPARAM wParam, LPARAM, BOOL&) {
  dpi_ = HIWORD(wParam) ? HIWORD(wParam) : _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  dwr_.reset();  // 重建字体资源以匹配新 DPI
  Reposition();
  Refresh();
  return 0;
}

LRESULT FamoStatusBar::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
  PAINTSTRUCT ps;
  ::BeginPaint(m_hWnd, &ps);
  DoPaint();
  ::EndPaint(m_hWnd, &ps);
  return 0;
}
