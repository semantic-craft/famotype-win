# 法墨输入法 Windows

本仓库是法墨输入法的独立 Windows 源码仓，只包含 Windows TSF、WinUI 设置面板、安装器源码、测试与 Windows CI。

- Windows 源码：`native/windows-tsf-famo/`
- Windows 安装包：[本仓 Releases](https://github.com/semantic-craft/famotype-win/releases/latest)
- macOS 源码：[famotype-macos](https://github.com/semantic-craft/famotype-macos)
- macOS 安装包：[famotype-macos releases](https://github.com/semantic-craft/famotype-macos/releases/latest)

Windows 与 macOS 的代码、tag、Latest Release 和发布资产相互独立，不得交叉上传。

当前 Windows 安装器构建入口为：

```powershell
native/windows-tsf-famo/installer/build-installer.ps1
```

构建与发布前必须按
[`native/windows-tsf-famo/installer/smoke_test.md`](native/windows-tsf-famo/installer/smoke_test.md)
完成身份检查、契约测试及 Win10/Win11 真机 smoke。macOS 机器不得伪造 Windows 二进制发布。

## License

法墨 Windows 包含基于 Weasel/RIME 的 GPL-3.0 组件。本仓按 GPL-3.0 发布；第三方来源与许可证见
[`THIRD-PARTY-NOTICES.txt`](native/windows-tsf-famo/installer/THIRD-PARTY-NOTICES.txt)。
