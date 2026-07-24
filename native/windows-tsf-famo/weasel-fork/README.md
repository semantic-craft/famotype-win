# weasel-fork — 法墨独立 TSF（基于 Weasel）

形态甲（PRD §6.1）：法墨 = 自带品牌的独立 TSF 输入法，自己的 CLSID/profile/server，配置写
`%LOCALAPPDATA%\Famo`，与用户已有 Rime/Weasel **并存、互不干扰、根本不碰 `%AppData%\Rime`**。

本目录是 **无 build 的源改写层**（本机无 MSVC/Boost，见 [[weasel-fork-toolchain]] 记忆 /
EXECUTION-PLAN S3.4）。它定义"法墨版 Weasel 怎么从上游 checkout 生成"，但不在此 build。

## 文件

| 文件 | 作用 | 切片 |
|---|---|---|
| `SOURCE.md` | 上游 pin + 许可 + 构建期取源步骤 | S3.1 |
| `famo-identity.json` | 法墨 identity 单一真相源（GUID/注册表/IPC/显示名） | S3.2 |
| `overlay/WeaselTSF/Globals.cpp` | 法墨 5 个 GUID（CLSID/profile/langbar/displayattr/preserved-key） | S3.2 |
| `apply-famo-identity.ps1` | 把上游 checkout 改写成法墨（overlay + 字符串替换，可 -DryRun） | S3.2 |
| `apply-famo-features.ps1` | 即时生效①②功能补丁（`features/instant-apply.patch`，可 -DryRun） | S4 |
| `apply-famo-statusbar.ps1` | 悬浮状态栏（overlay 新文件 + `features/status-bar.patch`，可 -DryRun） | status-bar |
| `overlay/include/FamoStatusBar.h` | 悬浮状态栏窗口类声明（阶段 1；纯 RECT，WeaselServer 可含） | status-bar |
| `overlay/include/FamoStatusBarInteraction.h` | 悬浮状态栏按钮 hover/press/cancel 纯交互模型（标准 C++ 可测） | status-bar |
| `overlay/WeaselUI/FamoStatusBar.cpp` | 悬浮状态栏 D2D 自绘实现（复用 WeaselPanel 原语，不抢焦点） | status-bar |
| `overlay/include/FamoPopupPanel.h` | 弹出功能面板窗口类声明（阶段 2；Item=label/action/checked 注入） | status-bar |
| `overlay/WeaselUI/FamoPopupPanel.cpp` | 弹出面板 D2D 自绘 + SetCapture 外部点击关闭 | status-bar |
| `overlay/resource/famo.png` | 状态栏展开按钮墨水滴 logo（运行期从 FamoRuntime.exe 同目录读） | status-bar |
| `features/status-bar.patch` | 既有文件改动：WeaselUI.vcxproj + WeaselServerApp.{h,cpp} + RimeWithWeasel.{h,cpp}（focus 回调） | status-bar |
| `features/highdpi-v2.patch` | 进程 DPI 感知升 PerMonitorV2：WeaselServer.cpp（动态 V2 回退 v1）+ WeaselServer.vcxproj（EnableDpiAwareness=false） | status-bar |
| `features/bounded-ipc-connect.patch` | WeaselIPC hot-path 边界：connect/read/write 1500ms 上限、overlapped I/O、timeout fail-open、畸形响应按协议失败 | stability |
| `seed-famo-config.ps1` | 首启 seed `famo-config/payload` → `%LOCALAPPDATA%\Famo` | S3.3 |
| `check-rime-untouched.ps1` | `%AppData%\Rime` 前后 hash 取证（证明只读不写） | S3.3 |

## apply 脚本运行顺序

`apply-famo-features.ps1`（即时生效）→ `apply-famo-statusbar.ps1`（状态栏）→ `apply-famo-identity.ps1`
（品牌/GUID）。status-bar patch 基于 features 改动后的 `WeaselServerApp.cpp` 生成，故须在 features 之后；
identity 串替换最后跑（对已是法墨值的文件命中 0 处，幂等）。

## 悬浮状态栏 + 弹出面板（status-bar · 阶段 1+2 已落地）

- **方案 A（已对照源码核验成立）**：状态栏/面板在 **WeaselServer 进程内** D2D 自绘——该进程已持
  `weasel::UI m_ui`（服务端渲染候选窗）、`RimeWithWeaselHandler m_handler`（含 `SetOption`）、托盘
  三者同进程，故开关**直达 `m_handler->SetOption(0, opt, val)`，零 IPC**。
- 复用 `WeaselPanel` 全套原语：不抢焦点 traits（`WS_EX_NOACTIVATE`+`WS_DISABLED`+`MA_NOACTIVATE`；
  状态栏可见态**不挂** `WS_EX_TOPMOST`，面板态**挂** TOPMOST 盖在栏上）、`DirectWriteResources`、
  `UpdateLayeredWindow` 逐像素 alpha。
- **状态栏（1A/1B）**：右下角圆角浮窗，5 按钮（中/Ａ·，/.·简/繁·半/全·☰），开态高亮；
  `ascii_mode`/`full_shape` 经 `OnUpdateUI`→`SyncStatus` 从真实 `m_ui.status()` 拉回。
- **交互闭环**：状态栏按钮具备 hover/press 视觉态；按下后拖出按钮再释放会取消点击，拖动状态栏仍只移动窗口。
  左键开关沿 `ToggleOption`/`SetOption` 真实生效并重绘。
- **弹出面板（阶段 2）**：☰ 点击 `Toggle` 弹出，竖排 4 开关（复用状态栏单一真相源）+ 设定/部署/退出；
  点击项执行后关闭；`SetCapture` 检测外部点击关闭，`WH_KEYBOARD_LL` 捕获 Esc 关闭。
- **右键菜单**：状态栏任意位置右键打开独立 `FamoPopupPanel`，只包含输入法设定、刷新配置、已接线开关、退出等真实动作；
  不展示语音、软键盘、扩展中心等未实现入口。
- 自验：x64 Release **编译零错误 + 静态库链接通过**（`FamoStatusBar.cpp`/`FamoPopupPanel.cpp` 入
  `WeaselUI.lib`，`WeaselServerApp.cpp` 接线编过）。最终 exe/dll 链接仅因**活跃 IME 占用旧产物
  （LNK1104）**未完成——停掉 dev IME 后即可，与 TSF 注册 + 可视化冒烟同属用户管理员/交互步骤。
- **焦点驱动显隐**（状态栏随焦点窗显隐 + 跟随定位）已落地：`RimeWithWeaselHandler` 加 focus 回调，
  FocusIn→`ShowOnFocus`（未拖过跟随前台窗右下角/拖过用持久化位）、FocusOut→`HideBar`。
- **高分屏（依据 `docs/sogou-highdpi-reverse-report.html`）**：法墨已是逐显示器自适应——
  `GetDpiForMonitor` + 每次显示重采样 DPI + 两窗 `WM_DPICHANGED` 重建/重定位 + D2D 原生 DIP，
  无需搜狗式"高分屏适配开关"（那是搜狗旧式 GDI 系统级 DPI 不可靠时的人工补救）。`features/highdpi-v2.patch`
  把进程感知由 PerMonitor v1 升 **V2**（非客户区/对话框/子窗口正确缩放、跨屏 WM_DPICHANGED 更完整），
  并把 `_Scale` 由截断改四舍五入、补面板 `OnDpiChanged` 重设尺寸（后两项在 overlay 自有文件内）。

## identity 改写覆盖（apply 脚本已验证）

- **GUID**：CLSID `54EAD76A-…`、profile `0158C2BA-…`、langbar、displayattr、preserved-key —— 全新生成，与 Weasel 不相交。
- **注册表品牌**：`Software\Rime\Weasel` → `Software\Famo\InputMethod`；RimeUserDir → `%LOCALAPPDATA%\Famo`；旧 `Software\Famo\Weasel` 只读迁移。
- **IPC 并存**：窗口 `WeaselIPCWindow_1.0`→`FamoIPCWindow_1.0`、管道 `WeaselNamedPipe`→`FamoNamedPipe`、互斥体 `WeaselDeployerMutex`→`FamoDeployerMutex`。
- **显示名**：`get_weasel_ime_name()` 小狼毫/Weasel → 法墨输入法/Famo Input Method；`WEASEL_CODE_NAME` → `Famo`。

## 构建期 TODO（C++ 工具链就绪后，S3.4）

1. **资源串**：`WeaselTSF/WeaselTSF.rc` 的 `小狼毫TSF`/`小狼毫`（FileDescription/ProductName）→ 法墨（需处理 .rc 编码，apply 脚本暂未改）。
2. **source project rename**：发布层已包装为 `FamoRuntime.exe` / `FamoTsf.dll` / `FamoDeploy.exe`；是否继续改 `.vcxproj` 原始输出名另开任务。
3. **build**：VS 2022 + C++（ATL/MFC）+ Boost ≥1.60；librime 用 `get-rime.ps1` 预编译。
4. **注册 + smoke**：开发版 profile 注册法墨，记事本 `nihao→你好`，确认日用 Weasel 不受影响。
