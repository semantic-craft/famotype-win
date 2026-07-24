# native/windows-tsf-famo —— 法墨 Windows 输入法（复用 Weasel + 现代化设置面板）

> 2026-06-26 起；当前源码与 ADR 为准。
> 方向：**复用 Weasel 的 TSF 热路径不重写**，主攻现代化设置面板 + 即时生效 + 独立自包含发行版。

## 它是什么

法墨 Windows 不是换皮 Weasel，也不是 RIME 配置包。它是一个**独立自包含的 Windows TSF 输入法**：
- 复用 `weasel.dll` 的 TSF/TIP 热路径（不碰，ADR-0001）。
- 在 `WeaselServer.exe` 现代化候选窗渲染（Direct2D，参照 Mozc BSD renderer）。
- 新建 **WinUI 3 设置面板**（对标微信/搜狗，参照 PowerToys Settings）。
- 配置只落 `%LOCALAPPDATA%\Famo`，与用户已有 Rime/Weasel 并存、不接管（形态甲）。
- 底座 = rime-ice（雾凇）`rime_ice` + 极点五笔 `wubi86_jidian`。整库 GPLv3。

## 目录结构（规划）

```
native/windows-tsf-famo/
  design/            # ✅ 已有：UI 设计系统本地副本（settings/FamoWindowsIME.html，已同步 claude.ai/design）
  famo-config/       # 🔜 Famo 配置 store 数据契约 + 从 rime-ice/五笔 组装 payload 的脚本（WSL 可做）
  settings-winui/    # 🔜 WinUI 3 设置面板工程（需 Windows: VS + Windows App SDK）
  weasel-fork/       # 🔜 Weasel 源码 fork：改注册 GUID/server 名 + RimeUserDir 覆盖 + 候选窗现代化（需 Windows: VS + C++）
  installer/         # 🔜 安装器：机器级装本体+TSF 注册（一次 UAC）+ 首启 per-user seed
```

## 三层职责（对应 PRD §2.1 架构脊柱）

| 层 | 进程 | 动/不动 | 工具链 |
|---|---|---|---|
| `weasel.dll` TSF TIP | 进程内热路径 | **复用，最小改动** | — |
| `WeaselServer.exe` 候选窗 | 独立进程 | 现代化 Direct2D 渲染 + 读 Famo store | Windows / VS C++ |
| Famo 设置 store | 注册表 / `%LOCALAPPDATA%\Famo` JSON | 新增 | 跨平台 |
| WinUI 3 设置面板 | 独立 app | 新建 | Windows / VS + WinAppSDK |

## 两档生效模型（PRD §2.3）

- **即时桶**（皮肤/字体/横竖排/圆角…）：设置面板写 store → 候选窗实时重绘，不部署。
- **部署桶**（方案/候选数/模糊音）：写 RIME yaml → 异步部署（预编译使切换≈秒级）。

## 构建前置（Windows 侧）

- Visual Studio 2022 + Desktop C++ + Windows App SDK（WinUI 3）。
- Weasel 源码（`rime/weasel`，GPL-3.0）+ librime。
- Inno Setup 6.7.3（已装本机）/ 或 MSIX。
- 本机已装 Weasel 0.17.4 可作行为参照（`C:\Program Files\Rime\weasel-0.17.4`）。

## 第一刀切片（walking skeleton）建议顺序

1. **`famo-config/`（WSL 可做，先做）**：定义 Famo 设置 store 的 JSON schema（即时桶字段 ↔ weasel.custom.yaml style 键；部署桶字段 ↔ default.custom.yaml）；写「从 rime-ice 取 rime_ice + 五笔 组装 payload」的脚本。
2. **`settings-winui/`（Windows）**：WinUI 3 空壳 + 侧栏导航跑通，照 design 稿铺一页（先「候选外观」即时桶），读写 store。
3. **`weasel-fork/`（Windows）**：fork Weasel，改注册 GUID/server 名 + RimeUserDir 指向 `%LOCALAPPDATA%\Famo`，能作为「法墨」独立输入法被添加、在记事本打字。
4. **联调**：设置面板改「字号/皮肤」→ 候选窗即时变（先走 Weasel reload，验证延迟，再决定是否升级到读 store 零延迟）。
5. **`installer/`**：机器级装 + 首启 seed，干净卸载 smoke。

> 现实约束：步骤 2/3/4/5 需在 Windows 机器（VS 工具链）进行；步骤 1 在当前 WSL 即可推进。
