# Famotype for Windows

[![Release](https://img.shields.io/github/v/release/semantic-craft/famotype-win?label=release)](https://github.com/semantic-craft/famotype-win/releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%2F%2011-black)](https://github.com/semantic-craft/famotype-win/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

**中文：[README.md](README.md)**

Famotype (法墨) for Windows is a self-contained TSF Chinese input method: it reuses the proven
Weasel / RIME engine and pairs it with a new WinUI 3 settings panel. It types out of the box, with
no configuration files to hand-edit.

**It coexists with an existing RIME / Weasel install.** Configuration lives only in
`%LOCALAPPDATA%\Famo`; Famotype never touches `%AppData%\Rime`, and uninstalling it leaves your
existing RIME setup alone.

## Install

Windows 10 / 11 (x64).

1. Download `Famo-Setup-<version>.exe` from the
   [latest release](https://github.com/semantic-craft/famotype-win/releases/latest).
2. Run it — machine-level install, one UAC prompt.
3. Deployment happens automatically the first time you enable 法墨. Switch to it in Notepad and
   start typing.

> **The installer is not Authenticode-signed** (the build machine has no code-signing
> certificate), so Windows SmartScreen may warn about an unrecognized app. Verify the SHA-256
> published in the release notes before running it.

If an old TSF DLL is still loaded by a running desktop app, the installer safely enters
`PendingReboot` and creates a one-user logon recovery task bound to the exact SID that started the
install. That task finishes the swap after a restart rather than force-closing your applications.

**Updates:** there is no background auto-update on Windows. The "检查更新" button on the settings
panel's About page opens this repository's Releases page; downloading and installing over the top
is your call.

## Features

- **Eight input schemas** — rime-ice full pinyin, six double-pinyin layouts (Flypy, Ziranma, MSPY,
  Sogou, Ziguang, Jiajia), Wubi 86 Jidian (plain, pinyin, traditional and traditional-pinyin), and
  T9.
- **A modern settings panel** — sidebar navigation with single-character badges plus grouped cards,
  instead of YAML.
- **Seven collegiate skins** — light/dark aware, switchable instantly.
- **Two-tier apply** — skins, fonts and horizontal/vertical layout take effect immediately; schema
  and fuzzy-pinyin changes deploy asynchronously, with no restart.
- **Candidate window that respects the system** — DPI, theme and high-contrast changes explicitly
  invalidate and repaint. High contrast uses system colors, a solid background and no shadow, with
  no animation or material blur.
- **Accessibility** — TSF `ITfCandidateListUIElement` remains the authoritative candidate semantics
  for UI-less and assistive clients; no new focusable window is introduced.

## Repository layout

- Windows source: `native/windows-tsf-famo/`
- Installers and matching source archives: this repository's
  [Releases](https://github.com/semantic-craft/famotype-win/releases)
- macOS edition: [famotype-macos](https://github.com/semantic-craft/famotype-macos) (source and
  installers both live there)

Code, tags, Latest Release and release assets are kept independent between Windows and macOS; the
two must never be cross-uploaded.

## Build from source

Installer entry point:

```powershell
native/windows-tsf-famo/installer/build-installer.ps1
```

Before building or releasing, complete the identity check, contract tests and a Win10 / Win11
on-device smoke run as described in
[`native/windows-tsf-famo/installer/smoke_test.md`](native/windows-tsf-famo/installer/smoke_test.md).
A macOS machine must never fabricate a Windows binary release.

The settings-panel tests are plain .NET and run anywhere:

```powershell
dotnet test native/windows-tsf-famo/settings-winui/FamoSettings.Tests
```

## License

Famotype for Windows includes GPL-3.0 components derived from Weasel / RIME, and this repository is
released under **GPL-3.0**; see [LICENSE](LICENSE). Third-party sources and licenses are listed in
[`THIRD-PARTY-NOTICES.txt`](native/windows-tsf-famo/installer/THIRD-PARTY-NOTICES.txt).

## Acknowledgements

- [RIME / Rime Input Method Engine](https://rime.im)
- [Weasel](https://github.com/rime/weasel)
- [rime-ice](https://github.com/iDvel/rime-ice)
- [Wubi 86 Jidian](https://github.com/KyleBing/rime-wubi86-jidian)
