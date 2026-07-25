# 法墨输入法 · Famotype for Windows

[![Release](https://img.shields.io/github/v/release/semantic-craft/famotype-win?label=release)](https://github.com/semantic-craft/famotype-win/releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%2F%2011-black)](https://github.com/semantic-craft/famotype-win/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

**English: [README.en.md](README.en.md)**

法墨 Windows 版是一个独立自包含的 TSF 中文输入法：复用成熟的 Weasel / RIME 引擎，
配一套全新的 WinUI 3 设置面板。装好就能打字，不用手写配置文件。

**与已装的 RIME / 小狼毫并存、互不干扰**——配置只落 `%LOCALAPPDATA%\Famo`，
从不触碰 `%AppData%\Rime`；卸载也不会动你原有的 RIME 配置。

## 安装

Windows 10 / 11（x64）。

1. 到 [Releases](https://github.com/semantic-craft/famotype-win/releases/latest) 下载
   `Famo-Setup-<版本>.exe`。
2. 双击运行，机器级安装，一次 UAC 授权。
3. 首次启用「法墨」时自动部署；在记事本里切到法墨即可打字。

> **安装包目前未做 Authenticode 代码签名**（发布机没有代码签名证书）。Windows SmartScreen
> 可能提示「未识别的应用」。请自行核对 Release 说明里公布的 SHA-256 后再决定是否运行。

如果旧版 TSF DLL 正被桌面应用加载，安装器会安全进入 `PendingReboot`，重启后由 RunOnce
完成切换，不会强制关掉你正在用的程序。

**更新**：Windows 版没有后台自动更新。设置面板「关于」页的「检查更新」会打开本仓库的
Releases 页面，由你决定是否下载新版覆盖安装。

## 特性

- **8 套输入方案**——雾凇拼音全拼、六种双拼（小鹤 / 自然码 / 微软 / 搜狗 / 紫光 / 加加）、
  极点五笔 86（含五笔拼音、繁体与繁体拼音四套）、T9。
- **现代化设置面板**——侧栏单字徽标导航（键 / 捷 / 候 / 皮 / 部 / 关）+ 分组卡片，
  不用翻 YAML。
- **4 套学院皮肤**——荔园红、胡佛红、珞珈青、嘉庚蓝，明暗自适应、即时换肤。
- **两档生效**——皮肤 / 字体 / 横竖排即时生效；方案 / 模糊音异步部署，不用重启。
- **候选窗贴合系统**——DPI、主题、高对比度变化会显式重绘；高对比度下用系统颜色、
  实色背景、无阴影，不加动画或材质模糊。
- **无障碍**——以 TSF `ITfCandidateListUIElement` 作为 UI-less / 辅助技术的权威候选语义，
  不新增会抢焦点的窗口。

## 仓库结构

- Windows 源码：`native/windows-tsf-famo/`
- 安装包与对应源码包：本仓库 [Releases](https://github.com/semantic-craft/famotype-win/releases)
- macOS 版：[famotype-macos](https://github.com/semantic-craft/famotype-macos)（源码与安装包都在那边）

Windows 与 macOS 的代码、tag、Latest Release 和发布资产相互独立，不得交叉上传。

## 从源码构建

安装器构建入口：

```powershell
native/windows-tsf-famo/installer/build-installer.ps1
```

构建与发布前必须按
[`native/windows-tsf-famo/installer/smoke_test.md`](native/windows-tsf-famo/installer/smoke_test.md)
完成身份检查、契约测试及 Win10 / Win11 真机 smoke。macOS 机器不得伪造 Windows 二进制发布。

设置面板的测试（纯 .NET，跨平台可跑）：

```powershell
dotnet test native/windows-tsf-famo/settings-winui/FamoSettings.Tests
```

## 许可

法墨 Windows 包含基于 Weasel / RIME 的 GPL-3.0 组件，本仓按 **GPL-3.0** 发布，
许可证全文见 [LICENSE](LICENSE)。第三方来源与许可证见
[`THIRD-PARTY-NOTICES.txt`](native/windows-tsf-famo/installer/THIRD-PARTY-NOTICES.txt)。

## 致谢

- [RIME / 中州韻输入法引擎](https://rime.im)
- [小狼毫 Weasel](https://github.com/rime/weasel)
- [雾凇拼音 rime-ice](https://github.com/iDvel/rime-ice)
- [极点五笔 86](https://github.com/KyleBing/rime-wubi86-jidian)
