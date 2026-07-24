#pragma once
// 法墨弹出功能面板（阶段 2）。
//
// 与 FamoStatusBar 同技术栈：WeaselServer 进程内、Direct2D 自绘、不抢焦点、LayeredWindow。
// 状态栏 ☰ 按钮点击 -> Toggle() 弹出/收起；竖排功能项（开关 + 动作），点击项执行回调后关闭。
// 外部点击经 SetCapture 检测关闭（无焦点窗口无法靠 WM_KILLFOCUS）。
//
// 项的动作/勾选态由 WeaselServerApp 注入（std::function），保持 WeaselUI 静态库与
// RimeWithWeasel/Server 解耦。开关项复用 FamoStatusBar 的 Toggle/Query 单一真相源。
//
// 红线：只活在 WeaselServer/WeaselUI 静态库内，绝不触碰 WeaselTSF 热路径。

#include "FamoStatusBarInteraction.h"

#include <WeaselUI.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// 面板展开时**临时置顶**（盖在状态栏之上）；保持 enabled 以接收行点击，
// 不抢焦点由 WS_EX_NOACTIVATE + OnMouseActivate 保证。
typedef ATL::CWinTraits<WS_POPUP | WS_CLIPSIBLINGS,
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                            WS_EX_TOPMOST>
    FamoPopupPanelTraits;

class FamoPopupPanel
    : public ATL::CWindowImpl<FamoPopupPanel, ATL::CWindow, FamoPopupPanelTraits> {
 public:
  DECLARE_WND_CLASS_EX(L"Famo_Popup_V1", CS_DBLCLKS, -1)

  // 一项：标签 + 点击动作 + 可选勾选态查询（checked 为空 => 非开关动作项）。
  struct Item {
    std::wstring label;
    std::function<void()> action;
    std::function<bool()> checked;  // 可空
  };

  FamoPopupPanel();
  ~FamoPopupPanel();

  void SetItems(std::vector<Item> items);  // WeaselServerApp 注入
  // 应用皮肤：从候选窗 UIStyle（famo-style.yaml 覆盖后）取色/圆角/字体，与候选窗+状态栏一致。
  void ApplySkin(const weasel::UIStyle& s);
  bool CreatePanel(HWND owner);            // 创建（默认隐藏）
  void Toggle(POINT anchorScreen);         // 开 -> 关 / 关 -> 开
  void ShowAt(POINT anchorScreen);         // anchor=状态栏左上屏幕坐标，面板定位其上方
  void HidePanel();
  void Refresh();                          // 经 UpdateLayeredWindow 重绘
  bool IsOpen() const { return m_hWnd && IsWindowVisible(); }

  BEGIN_MSG_MAP(FamoPopupPanel)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)  // -> MA_NOACTIVATE
  MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
  MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
  MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
  MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
  MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
  MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
  MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
  MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
  MESSAGE_HANDLER(WM_TIMER, OnTimer)
  MESSAGE_HANDLER(WM_PAINT, OnPaint)
  END_MSG_MAP()

  LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);      // hover 追踪
  LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);     // hover 离开清理
  LRESULT OnRButtonDown(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnRButtonUp(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDpiChanged(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);          // alpha 渐变步进
  LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);

 private:
  bool _PtInPanel(POINT pt) const;  // pt 是否落在面板客户区内（否则视为外部点击）
  void _BeginPress(POINT pt);
  LRESULT _EndPress(POINT pt);
  void _InvokeRow(int index);
  void DoPaint();
  void _LayerUpdate(const RECT& rc, HDC dc, BYTE alpha);
  void _InstallEscHook();    // 面板打开时装 WH_KEYBOARD_LL（无焦点窗口靠它收 Esc）
  void _RemoveEscHook();     // 面板关闭/销毁时卸载
  static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w, LPARAM l);
  void _EnsureResources();
  void _Layout();  // 按 DPI 算行 rc + 整窗尺寸 + 同步 interaction_ 命中矩形
  void _StartAlphaAnim(BYTE target);  // 打断式启动/改向 alpha 斜坡定时器
  void _BeginClose();  // 幂等：若可见且未在关闭中，启动渐出动画（供 HidePanel/OnCaptureChanged 共用）
  int _PanelWidthPx() const;
  int _PanelHeightPx() const;
  UINT _CurrentDpi() const;
  template <typename T>
  int _Scale(T v) const {
    // 四舍五入而非截断：125%/175% 等非整数倍率下避免 1px 误差累积。
    return static_cast<int>(v * dpi_scale_ + 0.5f);
  }

  std::vector<Item> items_;
  std::vector<RECT> rows_;  // 与 items_ 同序的行矩形（客户区像素）
  weasel::UIStyle style_;
  std::shared_ptr<weasel::DirectWriteResources> dwr_;

  // 皮肤色（ApplySkin 从 UIStyle 提取）。默认深色回退。
  D2D1_COLOR_F c_bg_ = D2D1::ColorF(0x2B2B2B, 0.97f);
  D2D1_COLOR_F c_text_ = D2D1::ColorF(0xE6E6E6, 1.0f);
  D2D1_COLOR_F c_check_ = D2D1::ColorF(0x4DA3FF, 1.0f);
  int round_corner_logical_ = 10;
  bool skin_applied_ = false;
  int skin_key_back_ = 0, skin_key_text_ = 0;
  std::wstring skin_key_font_;

  HHOOK m_esc_hook_ = NULL;       // 仅面板打开期间存活的低级键钩子
  static FamoPopupPanel* s_hook_owner_;  // 钩子回调 -> 该实例 HidePanel

  FamoStatusBarInteractionModel interaction_;  // hover/press 语义，与 FamoStatusBar 共用同一模型

  // 开合过渡：可打断的 alpha 渐变（定时器手动斜坡，非 AnimateWindow）。
  BYTE m_cur_alpha = 0xFF;
  BYTE m_target_alpha = 0xFF;
  UINT_PTR m_anim_timer = 0;
  bool m_closing_after_anim = false;
  static const UINT_PTR kAnimTimerId = 1;
  static const int kAlphaStep = 32;      // 每步步进量（0-255 尺度）
  static const UINT kAlphaAnimIntervalMs = 12;

  UINT dpi_ = 96;
  float dpi_scale_ = 1.0f;
  // 逻辑尺寸（96 DPI 基准）。
  static const int kRowH = 34;
  static const int kPadX = 14;
  static const int kPadY = 8;
  static const int kPanelW = 200;
};
