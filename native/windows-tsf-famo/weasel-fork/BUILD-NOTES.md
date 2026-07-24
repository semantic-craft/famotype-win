# weasel-fork · 构建实录（S3.4，工具链就绪后首次成功 build）

本机首次成功从源码构建法墨 Weasel fork（x64 Release）。记录工具链与避坑，供复现。
构建树是**一次性**的（`C:\fb`，不在 GPLv3 仓内）；本仓只存改写层（overlay/apply）+ 本笔记。

## 工具链（2026-06 本机实测）

| 组件 | 版本/路径 |
|---|---|
| VS Build Tools | **18** BuildTools `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` |
| MSVC 工具集 | **14.50.35717**（平台工具集 **v145**；`_MSC_VER` 1950） |
| Windows SDK | 10.0.26100.0 |
| C++ ATL | `Microsoft.VisualStudio.Component.VC.ATL`（**必装**；WTL/TSF/Server/Deployer 依赖） |
| Boost | **1.86.0**，仅 x64 静态（serialization/wserialization/thread/system/chrono），`/MT` runtime-link=static |
| librime | 预编译 dev（`get-rime.ps1 -use dev`，tag rime-33e7814 msvc x64/x86） |
| cmake | 4.3.3（boost 用 b2，不需要 cmake；仅记录） |

> **MFC 未装**：上游 `.rc` 仅为标准资源常量 include `afxres.h`。用 SDK 无 MFC 的 `winres.h`
> 做 shim 即可（见下），无需装 MFC。

## 复现步骤

```powershell
# 1) 取源 + 改写法墨 identity（含 %AppData%\Rime 回退改写到 %LOCALAPPDATA%\Famo）
git clone https://github.com/rime/weasel.git C:\fb\weasel
git -C C:\fb\weasel checkout 93eec2dc33dfcf04c356cce87732b638888fff4d
.\apply-famo-identity.ps1 -UpstreamDir C:\fb\weasel

# 2) 预编译 librime（放 include/ lib/ lib64/ output/）
cd C:\fb\weasel; mkdir lib,lib64,output\Win32,output\data\opencc -Force
pwsh -File .\get-rime.ps1 -use dev    # 注意：-extract 由 -use dev 自动开启，勿显式传 -extract

# 3) Boost（见 C:\fb\build-boost.bat）
# 4) 法墨 Weasel（见 C:\fb\build-weasel.bat）→ output\weaselx64.dll / WeaselServer.exe / WeaselDeployer.exe
#    安装器发布时包装为 FamoTsf.dll / FamoRuntime.exe / FamoDeploy.exe。
```

## 避坑（关键）

1. **`NoDefaultCurrentDirectoryInExePath=1`**（本机环境变量已置 1）会让 cmd 不在当前目录找命令，
   导致 boost `bootstrap.bat` 内部 `call guess_toolset.bat` 报“不是内部或外部命令”。
   构建脚本里先 `set "NoDefaultCurrentDirectoryInExePath="` 清空即可。

2. **Boost 引擎只认到 vc143**：`bootstrap.bat vc143`（用 14.50 的 cl 编译 b2，引擎只是需要可用的 cl）。

3. **b2 找不到 vcvarsall**：给 `--user-config=C:\fb\user-config.jam`，内容：
   `using msvc : 14.3 : "<...14.50...\cl.exe>" : <setup>"<...\VC\Auxiliary\Build\vcvarsall.bat>" ;`
   - 标 **14.3** → 产物 tag `vc143`，与 Boost auto-link 对 `_MSC_VER` 1950 的期望一致（实测链接通过，无 tag mismatch）。
   - `<setup>` 指向真实 vcvarsall，避免 b2 从 cl 路径瞎猜出不存在的 `Hostx64\vcvarsall.bat`。

4. **RC1015 找不到 `afxres.h`（MFC 未装）**：在 RC include 路径加 shim。
   - shim：`C:\fb\rcshim\afxres.h` = `#pragma once\n#include <winres.h>`（SDK 自带，定义 IDC_STATIC 等）。
   - 注入：`C:\fb\weasel\Directory.Build.targets`（**.targets 在工程体之后导入**，能对
     “显式设了 ResourceCompile/AdditionalIncludeDirectories 且不带 %(...) 继承”的 WeaselTSF 也生效；
     放 .props 只对带继承的 WeaselServer 有效，对 WeaselTSF 无效——实测如此）。

5. **源码构建输出的 TSF dll 名为 `weaselx64.dll`**（x64），不是 `WeaselTSF.dll`；安装器发布时复制为 `FamoTsf.dll`。Win32 为 `weasel.dll`。

## 产物校验（实测）

- `weaselx64.dll`（发布名 `FamoTsf.dll`）导出 `DllRegisterServer`/`DllGetClassObject`/`DllUnregisterServer`；
  含 `Software\Famo\InputMethod` ×1、`法墨输入法` ×1、`FamoNamedPipe` ×1；`Software\Rime\Weasel` ×0。
- 三个二进制均含 `%LOCALAPPDATA%\Famo` ×1、`%AppData%\Rime` ×0（回退已改写）。
- 引擎 smoke（`C:\fb\rime_smoke.c`，直连 rime.dll，user_dir=%LOCALAPPDATA%\Famo）：
  `nihao` → 候选 #1 = **你好**（共 6 候选）。`essay` db 缺失为无害告警（句子预测可选）。
- 部署只写 `%LOCALAPPDATA%\Famo\build`；`%AppData%\Rime` 当日无文件改动（日用 Rime 未受扰）。
