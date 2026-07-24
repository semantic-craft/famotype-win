# weasel-fork · 上游来源与许可（S3.1）

## 上游

- 仓库：**rime/weasel** — https://github.com/rime/weasel
- Pin commit：`93eec2dc33dfcf04c356cce87732b638888fff4d`（2026-03-06，`fix(WeaselTFS): Always update UIElements.`）
- 许可证：**GPL-3.0**（见上游 `LICENSE.txt`）。子模块 `librime`(BSD-3)、`plum`。

## 本 fork 的形态：overlay，不整库拷贝

法墨的差异极小（独立 TSF identity + 配置目录 + 品牌串），把整份 19MB Weasel 源码拷进本
GPLv3 仓库无意义。改为 **overlay + apply 脚本**：

- `famo-identity.json` —— 法墨 identity 单一真相源（5 个新 GUID、注册表品牌键、IPC 名、显示名）。
- `overlay/` —— 需整文件替换的上游文件：
    - `WeaselTSF/Globals.cpp` —— 法墨 GUID。
    - `resource/weasel.ico` —— 法墨品牌图标（深大红墨滴），覆盖上游 Weasel 图标。`WeaselTSF.rc` 的
      `IDI_WEASEL`（最低 ID = TSF profile 图标 index 0）即引用它，故构建后 weaselx64.dll 注册的输入法图标
      = 墨滴（与 macOS 版 Famo 对齐）。属 identity/品牌层，不动热路径逻辑。源自 macOS 主线仓 Rime.appiconset。
- `apply-famo-identity.ps1` —— 对一个干净的 rime/weasel checkout 施加 overlay + 精确字符串替换，
  产出"法墨版 Weasel"的 **identity**。可 `-DryRun`。
- `features/*.patch` + `apply-famo-features.ps1` —— 法墨**功能性**源码改动（与 identity 分开），
  `apply-famo-features.ps1` 按顺序 `git apply` 整组补丁：
    - `features/bounded-ipc-connect.patch` —— 历史文件名保留，但现在覆盖完整 WeaselIPC
      hot-path 边界：客户端连接不再 `WaitNamedPipe(500)` 无限循环（1500ms 总预算），读/写改为
      overlapped I/O + 1500ms wait + `CancelIo`，短响应/超大响应按 `ERROR_INVALID_DATA` 协议失败处理；
      `_Send` 对 `ERROR_SEM_TIMEOUT` 不再重连重试，直接抛回 `ClientImpl::_SendMessage` 的既有
      fail-open `return 0` 路径。触达 `WeaselIPC/PipeChannel.cpp` 与 `include/PipeChannel.h`。
    - `features/instant-apply.patch` —— 即时生效 ①开关 + ②外观（新 IPC
      `WEASEL_IPC_RELOAD_STYLE`/`WEASEL_IPC_RELOAD_OPTIONS`、WeaselServer 重读
      `famo-style.yaml`/`famo-options.yaml`、FamoDeploy `/reloadstyle`/`/reloadoptions`）。
      触达 8 文件，不碰 WeaselTSF 热路径逻辑。设计见 `design/INSTANT-APPLY-DESIGN.md`。
    - `features/launch-settings.patch` —— 托盘「设置」入口接 WinUI：新增 static helper
      `WeaselServerApp::launch_famo_settings(dir, page)`，托盘 `ID_WEASELTRAY_SETTINGS` 改为拉起
      `FamoSettings.exe --page input`（取代老 WeaselDeployer 配置对话框）。仅触达
      `WeaselServer/WeaselServerApp.{cpp,h}`，与 instant-apply / status-bar 区域正交。
    - `features/ensure-deployed.patch` —— 永不空配置兜底：`WeaselServerApp::ensure_deployed()` 在
      `Run()` 启动时检测 `%LOCALAPPDATA%\Famo\build\default.yaml` 缺失（安装器首启 /deploy 被静默安装
      /拒权跳过）→ 异步 `FamoDeploy /deploy`，从共享数据 `<exe>\data` 编译到用户 build，部署完
      经 IPC 通知本 server reload ⇒ **装完即用、首次输入即可用，无需手动部署**。仅触达
      `WeaselServer/WeaselServerApp.{cpp,h}`，与所有补丁正交。已实测：删 build → 重启 server → 自动编出 13 prism。
    - `features/select-schema.patch` —— 输入方式秒切（拼音/双拼/五笔）：新 IPC `WEASEL_IPC_SELECT_SCHEMA`、
      FamoDeploy `/selectschema`、`RimeWithWeaselHandler::SelectSchema`（读 `famo-select-schema.txt` →
      `rime_api->select_schema` 到各会话）+ `AddSession` 回放（重启保持），零部署、不重建 prism。触达 8 文件
      （与 instant-apply 同批 IPC 文件，**故针对「已应用 instant-apply」的树生成、须排其后**）。
    - `features/auto-pair.patch` —— 标点自动配对（搜狗式）：左符号上屏自动补右符号且光标居中
      （注入 `←` 方向键回退，先中和物理修饰键；TSF SetSelection 会被应用光标同步覆盖）、
      敲右符号且右侧已有时 type-over 越过（读文档核实，transitory 上下文退回 pending 状态判定）；开关
      `style/famo_auto_pair`（默认关）走 famo-style.yaml 热改，经 `config.famo_auto_pair=` IPC 行
      送 TSF 客户端。触达 6 文件（EditSession/KeyEventSink/WeaselTSF.h + WeaselIPCData.h/
      Configurator + RimeWithWeasel）。**这是首个触碰 WeaselTSF 提交热路径的功能补丁**（此前
      功能补丁只活在 server/UI 侧）；开关关闭时代码路径与上游等价。针对「features 1-4 已应用」
      的树生成、排最后。
    - `features/cjk-spacing.patch` —— 中英/中数字自动加空格：`WeaselTSF::DoEditSession` 里
      auto-pair 决策块之后再跑一遍 commit 边界分类（CJK/Latin/Digit，移植 macOS
      `FamoCommitTransform`），CJK↔Latin 或 CJK↔Digit 边界各自按开关插入一个空格，
      跨两次独立 commit 靠新成员 `_famo_prev_commit_boundary` 保留边界分类
      （不清零，同 auto-pair 的 `_famo_pending_close` 策略）。两个独立开关
      `style/famo_cjk_english_spacing`、`style/famo_cjk_number_spacing`（均默认关）走
      famo-style.yaml 热改，经 `config.famo_cjk_english_spacing=`/`config.famo_cjk_number_spacing=`
      两行 IPC 送 TSF 客户端（逐字段复制 `famo_auto_pair` 的形状，不做通用机制）。触达 5 文件
      （EditSession/WeaselTSF.h + WeaselIPCData.h/Configurator + RimeWithWeasel）。**第二个触碰
      WeaselTSF 提交热路径的功能补丁**；开关默认关闭时代码路径与 auto-pair 落地后的现状完全一致。
      针对「auto-pair.patch 已应用」的树生成、排最后。
  各补丁针对 pin commit 生成（select-schema 针对 instant-apply 已应用态，cjk-spacing 针对
  auto-pair 已应用态）。施加顺序见 `apply-famo-features.ps1`：
  `bounded-ipc-connect → instant-apply → launch-settings → ensure-deployed → select-schema → auto-pair → cjk-spacing`，
  随后 `apply-famo-statusbar.ps1` 的
  `status-bar → tray-options → highdpi-v2`。其中 `features/tray-options.patch` —— 托盘右键菜单顶部追加
  「中英文 / 中英标点 / 简繁 / 全半角」即时开关（macOS 菜单栏对齐）：`WeaselTrayIcon::CustomizeMenu` 动态
  `InsertMenu` + 勾选反映 `FamoStatusBar` 影子态，点击经 `ToggleOption` 直达 `SetOption`（零部署）。触达
  `WeaselTrayIcon.{cpp,h}` / `resource.h`×2 / `WeaselServerApp.cpp`，**依赖 status-bar 先行**（锚 `m_status_bar.Bind`）。
  `features/*.patch` 经 `.gitattributes` 标 `-text`（逐字节保存，勿做 EOL 归一化）。幂等由哨兵串
  `famo_cjk_number_spacing`（WeaselIPCData.h，仅 cjk-spacing 引入）判定（交错补丁使 reverse-check 不可靠）。
  构建从全新 pin checkout 应用一次。
- `seed-famo-config.ps1` / `check-rime-untouched.ps1` —— S3.3 配置 seed 与 %AppData%\Rime 取证。

构建时（C++ 工具链见 BUILD-NOTES.md）：
```powershell
git clone --recursive https://github.com/rime/weasel.git <dir>
git -C <dir> checkout 93eec2dc33dfcf04c356cce87732b638888fff4d
git -C <dir> submodule update --init --recursive
.\apply-famo-features.ps1 -UpstreamDir <dir>   # 先 features（git apply 补丁）
.\apply-famo-identity.ps1 -UpstreamDir <dir>   # 再 identity（串替换；对补丁带入的 2 文件幂等）
# 然后按上游 build.bat 流程构建（VS + Boost；librime 可用 get-rime.ps1 预编译）。
```
> 顺序固定 features→identity：补丁为 `WeaselIPC.h`/`RimeWithWeasel.cpp` 顺带带入它们的 identity 行
> （FamoNamedPipe/FamoDeployerMutex），故先打补丁、再跑 identity（该 2 文件命中 0 处，幂等）。

## 许可证合规（PRD §6.3）

- 本 fork 整体 **GPL-3.0**（继承 Weasel）。所有改写文件保留上游版权头；本仓不删上游 LICENSE。
- `apply-famo-identity.ps1` 只改 identity/品牌串，不动逻辑；功能性改动全部以 `features/*.patch`
  显式载明（其中 auto-pair、cjk-spacing 触及 WeaselTSF 提交热路径，见上）。
- 随安装器 `THIRD-PARTY-NOTICES` 须含：Weasel GPL-3.0、librime BSD、Mozc BSD-3（借鉴）、
  rime-ice（雾凇）GPL-3.0 + 其词库来源（腾讯 tencent / 字表 8105·41448 等）、
  wubi86-jidian Apache-2.0（见 S5.1）。
