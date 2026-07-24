#pragma once
// 法墨悬浮状态栏（阶段 1B：完整状态栏）。
//
// 复用候选窗 WeaselPanel 的全套不抢焦点 + LayeredWindow + Direct2D 自绘原语，
// 在 WeaselServer 进程内创建一个常驻悬浮状态栏。开关按钮点击后通过注入的
// OptionSetter 回调直达同进程的 RimeWithWeaselHandler::SetOption（零 IPC）。
// 真实 Rime 状态（ascii_mode/full_shape）经 WeaselServer 的 OnUpdateUI 回调 SyncStatus
// 同步回来（Rime 无 GetOption；ascii_punct/traditionalization/zh_trad 不在 weasel::Status，故仍用影子态）。
//
// 设计/依据：docs/sogou-status-bar-reverse-report.html 方案 A；
//
// 红线：本窗口只活在 WeaselServer/WeaselUI 静态库内，绝不触碰 WeaselTSF 热路径。

#include "FamoStatusBarInteraction.h"

#include <WeaselUI.h>  // weasel::UIStyle / weasel::DirectWriteResources

#include <functional>
#include <memory>
#include <string>
#include <vector>

// 可见悬浮状态栏样式：复用候选窗 traits，保持 topmost + enabled，让左键点击能进入
// 按钮命中/切换链路；不抢焦点由 WS_EX_NOACTIVATE + OnMouseActivate 保证。
typedef ATL::CWinTraits<WS_POPUP | WS_CLIPSIBLINGS,
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                            WS_EX_TOPMOST>
    FamoStatusBarTraits;

class FamoStatusBar
    : public ATL::CWindowImpl<FamoStatusBar, ATL::CWindow, FamoStatusBarTraits> {
 public:
  DECLARE_WND_CLASS_EX(L"Famo_StatusBar_V1", CS_DBLCLKS, -1)

  // 由 WeaselServerApp 注入：opt/val -> m_handler->SetOption(0, opt, val)。
  // 用回调而非直接依赖 RimeWithWeasel.h，保持 WeaselUI 静态库解耦。
  using OptionSetter = std::function<void(const std::string& opt, bool val)>;

  FamoStatusBar();
  ~FamoStatusBar();

  void Bind(OptionSetter setter) { setter_ = std::move(setter); }
  // 展开按钮回调（阶段 2 接弹出面板；1B 未接时点击无副作用）。
  void OnExpand(std::function<void()> cb) { on_expand_ = std::move(cb); }
  using RightClickHandler = std::function<void(POINT anchorScreen)>;
  void OnRightClick(RightClickHandler cb) { on_right_click_ = std::move(cb); }

  bool CreateBar(HWND owner);  // 创建（默认隐藏，需 ShowBar/ShowOnFocus）
  void ShowBar();              // Reposition + Refresh + ShowWindow(SW_SHOWNOACTIVATE)
  void ShowOnFocus();          // 焦点驱动显示：未拖过则固定屏幕右下角，拖过则用持久化位
  void HideBar();
  // 拖动位置持久化文件路径（如 %LOCALAPPDATA%\Famo\famo-statusbar.txt）。
  void SetStatePath(const std::wstring& path);
  void Reposition();           // 主显示器工作区右下角
  void Refresh();              // 经 UpdateLayeredWindow 重绘
  // 从真实 Rime 状态同步两个开关（无 GetOption，经 WeaselServer OnUpdateUI 调）。
  void SyncStatus(bool ascii_mode, bool full_shape);

  // 供弹出面板复用的单一真相源（按选项名翻转/查询本栏开关），及面板锚点。
  void ToggleOption(const std::string& opt);       // 翻转匹配按钮 + setter + 重绘
  bool QueryOption(const std::string& opt) const;  // 读影子态（无匹配返回 false）
  POINT AnchorTopLeftScreen() const;               // 状态栏左上角屏幕坐标

  // 应用皮肤：从候选窗 UIStyle（famo-style.yaml 覆盖后）取色/圆角/字体，与候选窗一致。
  // 带变更检测，可每次 OnUpdateUI 调用而不触发重绘风暴。
  void ApplySkin(const weasel::UIStyle& s);

  BEGIN_MSG_MAP(FamoStatusBar)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
  MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
  MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
  MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
  MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
  MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
  MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
  MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
  MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
  MESSAGE_HANDLER(WM_PAINT, OnPaint)
  END_MSG_MAP()

  LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&);  // -> MA_NOACTIVATE
  LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&);      // -> HTCLIENT
  LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);    // 起拖
  LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);      // 拖动跟随
  LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);      // 抬起：拖拽结束 or 按钮点击
  LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);     // hover 离开清理
  LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&); // 捕获中断清理
  LRESULT OnRButtonDown(UINT, WPARAM, LPARAM, BOOL&);    // 吞掉默认菜单
  LRESULT OnRButtonUp(UINT, WPARAM, LPARAM, BOOL&);      // 右键菜单
  LRESULT OnDpiChanged(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);

 private:
  enum class ButtonAction { None, Expand };

  // 一个可点击区域：开关（option 非空 + state）或动作（option 空，如展开）。
  // 用纯 RECT（非 WTL CRect）：本头被 WeaselServer 包含，其预编译头不含 WTL atlmisc。
  struct Button {
    RECT rc{};
    std::string option;           // Rime 选项名；空 => 非开关（展开按钮）
    bool* state = nullptr;        // 开关影子态指针；非开关为 null
    const wchar_t* label0 = L"";  // state=false / 非开关唯一标签
    const wchar_t* label1 = L"";  // state=true
    ButtonAction action = ButtonAction::None;
  };

  void DoPaint();          // 离屏 DC -> D2D BindDC/BeginDraw/EndDraw -> _LayerUpdate
  void _LayerUpdate(const RECT& rc, HDC dc);  // 照搬 WeaselPanel::_LayerUpdate
  void _EnsureResources();  // 惰性建 DirectWriteResources（自有，不复用候选窗的）
  void _BuildButtons();     // 建按钮规格（选项/标签/影子态指针），一次
  void _LayoutButtons();    // 按当前 DPI 重算各按钮 rc
  int _BarWidthPx() const;  // 由按钮数算窗口像素宽
  int _BarHeightPx() const;
  void _SavePos();          // 拖动后写当前左上屏幕坐标到 state_path_
  void _LoadPos();          // 启动时读回；命中则 m_user_moved=true + 定位（夹在显示器内）
  void _ClampToMonitor(int& x, int& y, int w, int h) const;  // 夹到最近显示器工作区
  void _DrawExpandGlyph(const RECT& button_rc, bool hover, bool press);
  UINT _CurrentDpi() const;
  template <typename T>
  int _Scale(T v) const {
    // 四舍五入而非截断：125%/175% 等非整数倍率下避免 1px 误差累积。
    return static_cast<int>(v * dpi_scale_ + 0.5f);
  }

  OptionSetter setter_;
  std::function<void()> on_expand_;
  RightClickHandler on_right_click_;
  weasel::UIStyle style_;  // 仅供 DWR 文本格式用（DWR 持其引用，故为成员）
  std::shared_ptr<weasel::DirectWriteResources> dwr_;
  FamoStatusBarInteractionModel interaction_;

  // 皮肤色（由 ApplySkin 从 UIStyle 提取；0xAABBGGRR -> D2D）。默认深色回退。
  D2D1_COLOR_F c_bg_ = D2D1::ColorF(0x2B2B2B, 0.94f);
  D2D1_COLOR_F c_text_ = D2D1::ColorF(0xCCCCCC, 1.0f);
  D2D1_COLOR_F c_hi_bg_ = D2D1::ColorF(0x3B6EA5, 0.95f);
  D2D1_COLOR_F c_hi_text_ = D2D1::ColorF(0xFFFFFF, 1.0f);
  int round_corner_logical_ = 8;
  // 皮肤变更检测（避免每次 OnUpdateUI 重建资源）。
  bool skin_applied_ = false;
  int skin_key_back_ = 0, skin_key_text_ = 0;
  std::wstring skin_key_font_;

  std::vector<Button> buttons_;
  // 影子态：ascii_mode/full_shape 会被 SyncStatus 用真实 Rime 状态覆盖；
  // ascii_punct/traditionalization/zh_trad 不在 weasel::Status，点击后仅本地翻转。
  // 简繁显示共用 traditionalization 影子态，下发时同时设置五笔方案使用的 zh_trad。
  bool opt_ascii_mode_ = false;
  bool opt_ascii_punct_ = false;
  bool opt_traditionalization_ = false;
  bool opt_full_shape_ = false;

  // 拖动状态（像搜狗状态栏那样悬浮可拖）。
  bool m_maybe_drag = false;  // 已按下、尚未判定为拖拽
  bool m_dragging = false;    // 已超过阈值，确认拖拽中
  bool m_user_moved = false;  // 用户拖动过/有持久化位 -> 不再自动回右下角
  POINT m_drag_start = {0, 0};  // 按下时光标屏幕坐标
  POINT m_win_start = {0, 0};   // 按下时窗口左上屏幕坐标
  std::wstring state_path_;     // 拖动位置持久化文件

  UINT dpi_ = 96;
  float dpi_scale_ = 1.0f;
  // 逻辑尺寸（96 DPI 基准，绘制时按 dpi_scale_ 放大）。
  static const int kCell = 34;  // 单按钮格逻辑宽/高
  static const int kGap = 2;    // 按钮间距
  static const int kPad = 8;    // 左右内边距
  int bar_h_logical_ = 44;
};
