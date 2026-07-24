#include "stdafx.h"

#include "FamoPopupPanel.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <shellscalingapi.h>

#include <cmath>  // fabsf

#pragma comment(lib, "Shcore.lib")

// 阶段 2：竖排功能面板。开关项右侧画勾；动作项无勾。点击项执行 action 后关闭。
// 外部点击：ShowAt 时 SetCapture，OnLButtonUp 若落在某行 rc 内执行该行，否则视为外部点击关闭。

namespace {
inline D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.0f) {
  return D2D1::ColorF(rgb, a);
}
// Rime/Weasel 颜色 0xAABBGGRR -> D2D1::ColorF（R=低字节，alpha=高字节）。
inline D2D1_COLOR_F FromRimeColor(int c) {
  return D2D1::ColorF((c & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
                      ((c >> 16) & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f);
}
inline D2D1_COLOR_F PickColor(int c, D2D1_COLOR_F fallback) {
  return ((c >> 24) & 0xff) ? FromRimeColor(c) : fallback;
}
inline float Lum(const D2D1_COLOR_F& c) {
  return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}
inline D2D1_COLOR_F Legible(D2D1_COLOR_F text, const D2D1_COLOR_F& bg) {
  text.a = 1.0f;
  if (fabsf(Lum(text) - Lum(bg)) < 0.4f)
    return (Lum(bg) > 0.5f) ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)
                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
  return text;
}
}  // namespace

void FamoPopupPanel::ApplySkin(const weasel::UIStyle& s) {
  if (skin_applied_ && s.back_color == skin_key_back_ &&
      s.candidate_text_color == skin_key_text_ && s.font_face == skin_key_font_)
    return;
  skin_applied_ = true;
  skin_key_back_ = s.back_color;
  skin_key_text_ = s.candidate_text_color;
  skin_key_font_ = s.font_face;

  c_bg_ = PickColor(s.back_color, Rgb(0x2B2B2B, 0.97f));
  c_text_ = PickColor(
      s.candidate_text_color ? s.candidate_text_color : s.text_color,
      Rgb(0xE6E6E6, 1.0f));
  c_check_ = PickColor(s.hilited_candidate_back_color
                           ? s.hilited_candidate_back_color
                           : s.hilited_back_color,
                       Rgb(0x4DA3FF, 1.0f));
  c_text_ = Legible(c_text_, c_bg_);  // 行文字保证与面板背景对比
  round_corner_logical_ = s.round_corner > 0 ? s.round_corner : 10;

  if (!s.font_face.empty()) {
    style_.font_face = s.font_face;
    style_.label_font_face = s.font_face;
    style_.comment_font_face = s.font_face;
    dwr_.reset();
  }
  Refresh();
}

FamoPopupPanel* FamoPopupPanel::s_hook_owner_ = nullptr;

LRESULT CALLBACK FamoPopupPanel::LowLevelKeyboardProc(int code,
                                                      WPARAM wParam,
                                                      LPARAM lParam) {
  if (code == HC_ACTION &&
      (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
    auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (k && k->vkCode == VK_ESCAPE && s_hook_owner_ &&
        s_hook_owner_->IsOpen()) {
      s_hook_owner_->HidePanel();
      return 1;  // 吞掉这次 Esc（用于关面板，不再下发）
    }
  }
  return ::CallNextHookEx(NULL, code, wParam, lParam);
}

void FamoPopupPanel::_InstallEscHook() {
  if (m_esc_hook_)
    return;
  s_hook_owner_ = this;
  // LL 键钩子在安装线程的消息循环上回调（WeaselServer 主线程有循环）。
  m_esc_hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &LowLevelKeyboardProc,
                                    ::GetModuleHandleW(NULL), 0);
}

void FamoPopupPanel::_RemoveEscHook() {
  if (m_esc_hook_) {
    ::UnhookWindowsHookEx(m_esc_hook_);
    m_esc_hook_ = NULL;
  }
  if (s_hook_owner_ == this)
    s_hook_owner_ = nullptr;
}

FamoPopupPanel::FamoPopupPanel() {
  style_.font_face = L"Microsoft YaHei UI";
  style_.label_font_face = style_.font_face;
  style_.comment_font_face = style_.font_face;
  style_.font_point = 12;
  style_.label_font_point = 12;
  style_.comment_font_point = 12;
}

FamoPopupPanel::~FamoPopupPanel() {}

void FamoPopupPanel::SetItems(std::vector<Item> items) {
  items_ = std::move(items);
  _Layout();
}

UINT FamoPopupPanel::_CurrentDpi() const {
  HMONITOR mon = m_hWnd
                     ? ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY)
                     : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  UINT dx = 96, dy = 96;
  if (SUCCEEDED(::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx)
    return dx;
  return 96;
}

int FamoPopupPanel::_PanelWidthPx() const {
  return _Scale(kPanelW);
}

int FamoPopupPanel::_PanelHeightPx() const {
  const int n = static_cast<int>(items_.size());
  return _Scale(kPadY) * 2 + n * _Scale(kRowH);
}

void FamoPopupPanel::_EnsureResources() {
  if (dwr_)
    return;
  dwr_ = std::make_shared<weasel::DirectWriteResources>(style_, dpi_);
  dwr_->InitResources(style_, dpi_);
  // 行内文字：左对齐、垂直居中。勾选标记另用居中右侧绘制。
  if (dwr_->pTextFormat) {
    dwr_->pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    dwr_->pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
}

void FamoPopupPanel::_Layout() {
  rows_.clear();
  const int w = _PanelWidthPx();
  const int rowH = _Scale(kRowH);
  int top = _Scale(kPadY);
  for (size_t i = 0; i < items_.size(); ++i) {
    rows_.push_back(RECT{0, top, w, top + rowH});
    top += rowH;
  }
  std::vector<FamoStatusBarRect> rects;
  rects.reserve(rows_.size());
  for (const auto& r : rows_)
    rects.push_back({r.left, r.top, r.right, r.bottom});
  interaction_.SetButtonRects(std::move(rects));
}

bool FamoPopupPanel::CreatePanel(HWND owner) {
  dpi_ = _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  _Layout();
  RECT init = {0, 0, _PanelWidthPx(), _PanelHeightPx()};
  HWND hwnd = Create(owner, &init, NULL, 0, 0, 0U, NULL);
  return hwnd != NULL;
}

void FamoPopupPanel::ShowAt(POINT anchorScreen) {
  if (!m_hWnd)
    return;
  dpi_ = _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  _Layout();
  const int w = _PanelWidthPx();
  const int h = _PanelHeightPx();
  // 定位在锚点（状态栏左上）上方，gap 间距；夹在显示器工作区内。
  const int gap = _Scale(6);
  int x = anchorScreen.x;
  int y = anchorScreen.y - h - gap;
  HMONITOR mon = ::MonitorFromPoint(anchorScreen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(MONITORINFO)};
  if (::GetMonitorInfo(mon, &mi)) {
    if (x + w > mi.rcWork.right)
      x = mi.rcWork.right - w;
    if (x < mi.rcWork.left)
      x = mi.rcWork.left;
    if (y < mi.rcWork.top)
      y = anchorScreen.y + gap;  // 上方放不下则放下方
  }
  SetWindowPos(HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

  // 打断任何进行中的关闭动画，直接进入开启序列（避免"点了没反应"的等待感）。
  if (m_anim_timer) {
    ::KillTimer(m_hWnd, m_anim_timer);
    m_anim_timer = 0;
  }
  m_closing_after_anim = false;
  m_cur_alpha = 0x40;  // 起始较透明但非全透明，避免"先黑一下"的闪烁
  ShowWindow(SW_SHOWNOACTIVATE);
  Refresh();
  ::SetCapture(m_hWnd);  // 捕获鼠标以检测外部点击关闭
  _InstallEscHook();     // 装键钩子收 Esc
  _StartAlphaAnim(0xFF);  // 渐入到全不透明
}

void FamoPopupPanel::HidePanel() {
  _RemoveEscHook();
  if (!m_hWnd)
    return;
  if (::GetCapture() == m_hWnd)
    ::ReleaseCapture();  // 输入语义立即生效，不等动画（会同步触发 WM_CAPTURECHANGED）
  _BeginClose();
}

void FamoPopupPanel::_BeginClose() {
  if (!IsWindowVisible() || m_closing_after_anim)
    return;  // 已隐藏或已在关闭动画中，避免重复触发
  _RemoveEscHook();
  m_closing_after_anim = true;
  _StartAlphaAnim(0x00);  // 渐出后由 OnTimer 收尾 ShowWindow(SW_HIDE)
}

void FamoPopupPanel::_StartAlphaAnim(BYTE target) {
  m_target_alpha = target;
  if (!m_anim_timer)
    m_anim_timer = ::SetTimer(m_hWnd, kAnimTimerId, kAlphaAnimIntervalMs, nullptr);
}

void FamoPopupPanel::Toggle(POINT anchorScreen) {
  if (IsOpen())
    HidePanel();
  else
    ShowAt(anchorScreen);
}

void FamoPopupPanel::Refresh() {
  if (m_hWnd)
    DoPaint();
}

void FamoPopupPanel::DoPaint() {
  ModifyStyleEx(WS_EX_TRANSPARENT, WS_EX_LAYERED);
  CRect rc;
  GetClientRect(&rc);
  if (rc.Width() <= 0 || rc.Height() <= 0)
    return;
  if (rows_.size() != items_.size())
    _Layout();

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
  rt->Clear(Rgb(0x000000, 0.0f));

  const float radius = static_cast<float>(_Scale(round_corner_logical_));
  D2D1_ROUNDED_RECT bg = {D2D1::RectF(0.5f, 0.5f, (float)rc.Width() - 0.5f,
                                      (float)rc.Height() - 0.5f),
                          radius, radius};
  dwr_->CreateBrush(c_bg_);
  rt->FillRoundedRectangle(bg, dwr_->pBrush.Get());

  const int padx = _Scale(kPadX);
  const float checkW = static_cast<float>(_Scale(22));
  for (size_t i = 0; i < items_.size() && i < rows_.size(); ++i) {
    const Item& it = items_[i];
    const RECT& row = rows_[i];
    const bool isToggle = (bool)it.checked;
    const bool on = isToggle && it.checked();
    const bool hover = (interaction_.HoverIndex() == static_cast<int>(i));
    const bool press = (interaction_.PressIndex() == static_cast<int>(i)) &&
                       !interaction_.PressOutside();

    // hover/press 行高亮：复用 c_check_ 色相，仅调 alpha（不新增皮肤字段）。
    if (hover || press) {
      D2D1_COLOR_F fill = c_check_;
      fill.a = press ? 0.22f : 0.12f;
      const float hlRound = static_cast<float>(_Scale(6));
      D2D1_ROUNDED_RECT hl = {
          D2D1::RectF((float)row.left, (float)row.top, (float)row.right,
                      (float)row.bottom),
          hlRound, hlRound};
      dwr_->CreateBrush(fill);
      rt->FillRoundedRectangle(hl, dwr_->pBrush.Get());
    }

    // 标签（左对齐，留出右侧勾选区）。
    dwr_->SetBrushColor(c_text_);
    if (dwr_->pTextFormat && !it.label.empty()) {
      const float lw = (float)(row.right - row.left) - padx * 2 - checkW;
      const float lh = (float)(row.bottom - row.top);
      dwr_->CreateTextLayout(it.label, (UINT32)it.label.size(),
                             dwr_->pTextFormat.Get(), lw, lh);
      dwr_->DrawTextLayoutAt(D2D1::Point2F((float)(row.left + padx),
                                           (float)row.top));
      dwr_->ResetLayout();
    }
    // 开关勾选标记（右侧），开=强调色 ✓，关=不画。
    if (isToggle && on && dwr_->pTextFormat) {
      dwr_->SetBrushColor(c_check_);
      const wchar_t* mark = L"✓";  // ✓
      dwr_->CreateTextLayout(mark, 1, dwr_->pTextFormat.Get(), checkW,
                             (float)(row.bottom - row.top));
      dwr_->DrawTextLayoutAt(
          D2D1::Point2F((float)(row.right - padx) - checkW, (float)row.top));
      dwr_->ResetLayout();
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
  _LayerUpdate(rc, memDC, m_cur_alpha);

  ::SelectObject(memDC, oldBmp);
  ::DeleteObject(memBmp);
  ::DeleteDC(memDC);
}

void FamoPopupPanel::_LayerUpdate(const RECT& rc, HDC dc, BYTE alpha) {
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
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
  ::UpdateLayeredWindow(m_hWnd, screen, &pos, &sz, dc, &src, RGB(0, 0, 0), &bf,
                        ULW_ALPHA);
  ::ReleaseDC(NULL, screen);
}

LRESULT FamoPopupPanel::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  _Layout();
  return 0;
}

LRESULT FamoPopupPanel::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
  if (m_anim_timer) {
    ::KillTimer(m_hWnd, m_anim_timer);  // 避免销毁后野指针回调
    m_anim_timer = 0;
  }
  _RemoveEscHook();
  dwr_.reset();
  return 0;
}

LRESULT FamoPopupPanel::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL& bHandled) {
  bHandled = TRUE;
  return MA_NOACTIVATE;
}

LRESULT FamoPopupPanel::OnNcHitTest(UINT, WPARAM, LPARAM, BOOL& bHandled) {
  bHandled = TRUE;
  return HTCLIENT;
}

bool FamoPopupPanel::_PtInPanel(POINT pt) const {
  RECT client = {0, 0, 0, 0};
  return m_hWnd && ::GetClientRect(m_hWnd, &client) && ::PtInRect(&client, pt);
}

void FamoPopupPanel::_BeginPress(POINT pt) {
  interaction_.LeftDown({pt.x, pt.y});
  if (::GetCapture() != m_hWnd)
    ::SetCapture(m_hWnd);
  if (interaction_.PressIndex() >= 0)
    Refresh();
}

void FamoPopupPanel::_InvokeRow(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size()))
    return;
  auto action = items_[index].action;  // copy before HidePanel closes capture
  HidePanel();
  if (action)
    action();
}

LRESULT FamoPopupPanel::_EndPress(POINT pt) {
  if (!_PtInPanel(pt)) {
    HidePanel();  // 落点在面板外 -> 外部点击，直接关闭（不触发任何行动作）
    return 0;
  }
  FamoStatusBarClick click =
      interaction_.LeftUp({pt.x, pt.y}, /*was_drag=*/false);
  if (click.has_value) {
    _InvokeRow(click.index);
    return 0;
  }
  Refresh();  // 清理 press 视觉态（如落在行间空白）
  return 0;
}

LRESULT FamoPopupPanel::OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  _BeginPress(pt);
  return 0;
}

LRESULT FamoPopupPanel::OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL&) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  const bool changed = interaction_.MouseMove({pt.x, pt.y});
  if (interaction_.HoverIndex() >= 0 || interaction_.PressIndex() >= 0) {
    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT)};
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = m_hWnd;
    ::TrackMouseEvent(&tme);
  }
  if (changed)
    Refresh();
  return 0;
}

LRESULT FamoPopupPanel::OnLButtonUp(UINT, WPARAM, LPARAM lParam, BOOL&) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  return _EndPress(pt);
}

LRESULT FamoPopupPanel::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&) {
  if (interaction_.MouseLeave())
    Refresh();
  return 0;
}

LRESULT FamoPopupPanel::OnRButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  _BeginPress(pt);
  return 0;
}

LRESULT FamoPopupPanel::OnRButtonUp(UINT, WPARAM, LPARAM lParam, BOOL&) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  return _EndPress(pt);
}

LRESULT FamoPopupPanel::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&) {
  if (interaction_.CaptureChanged())
    Refresh();
  // 捕获被夺走（外部点击/其他窗口 SetCapture，或 HidePanel 主动 ReleaseCapture 触发）
  // -> 走渐出动画收起。_BeginClose 幂等，HidePanel 随后的调用会安全跳过。
  _BeginClose();
  return 0;
}

LRESULT FamoPopupPanel::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL&) {
  if (wParam != kAnimTimerId)
    return 0;
  int next = m_cur_alpha;
  if (next < m_target_alpha) {
    next += kAlphaStep;
    if (next > m_target_alpha)
      next = m_target_alpha;
  } else if (next > m_target_alpha) {
    next -= kAlphaStep;
    if (next < m_target_alpha)
      next = m_target_alpha;
  }
  m_cur_alpha = static_cast<BYTE>(next);
  Refresh();
  if (m_cur_alpha == m_target_alpha) {
    ::KillTimer(m_hWnd, m_anim_timer);
    m_anim_timer = 0;
    if (m_closing_after_anim) {
      m_closing_after_anim = false;
      ShowWindow(SW_HIDE);
    }
  }
  return 0;
}

LRESULT FamoPopupPanel::OnDpiChanged(UINT, WPARAM wParam, LPARAM, BOOL&) {
  dpi_ = HIWORD(wParam) ? HIWORD(wParam) : _CurrentDpi();
  dpi_scale_ = dpi_ / 96.0f;
  dwr_.reset();
  _Layout();
  // 随 DPI 重设窗口像素尺寸（_Layout 只改行 rc，窗口本身需 SetWindowPos 跟上）。
  // 保位不移（SWP_NOMOVE）：面板锚定状态栏、生命周期短，跨屏时下次 ShowAt 会重算定位。
  SetWindowPos(NULL, 0, 0, _PanelWidthPx(), _PanelHeightPx(),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  Refresh();
  return 0;
}

LRESULT FamoPopupPanel::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
  PAINTSTRUCT ps;
  ::BeginPaint(m_hWnd, &ps);
  DoPaint();
  ::EndPaint(m_hWnd, &ps);
  return 0;
}
