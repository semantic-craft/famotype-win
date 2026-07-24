#requires -version 5
<#
.SYNOPSIS
  在干净的 Weasel checkout 上施加法墨「悬浮状态栏 + 弹出面板」功能（阶段 1+2）。

.DESCRIPTION
  与 apply-famo-identity.ps1（品牌/GUID 串替换）、apply-famo-features.ps1（即时生效 IPC）分工：
  本脚本落地状态栏功能源码改动，分两步——

    1) overlay 拷入全新源文件：
       overlay/include/FamoStatusBar.h   -> include/FamoStatusBar.h
       overlay/include/FamoStatusBarInteraction.h -> include/FamoStatusBarInteraction.h
       overlay/WeaselUI/FamoStatusBar.cpp -> WeaselUI/FamoStatusBar.cpp
       （阶段 2 增 FamoPopupPanel.{h,cpp} 时一并加入此列表）

    2) git apply 两个补丁（按序，单个幂等）落地对既有文件的改动：
       features/status-bar.patch（5 文件）：
       - WeaselUI/WeaselUI.vcxproj         （新增 ClCompile/ClInclude 条目）
       - WeaselServer/WeaselServerApp.h    （新增 #include + FamoStatusBar/Popup 成员）
       - WeaselServer/WeaselServerApp.cpp  （创建/显示/销毁 + SetOption + 焦点接线）
       - RimeWithWeasel/RimeWithWeasel.cpp + include/RimeWithWeasel.h（focus 回调）
       features/highdpi-v2.patch（2 文件）：进程 DPI 感知升级到 PerMonitorV2
       - WeaselServer/WeaselServer.cpp     （SetupProcessDpiAwareness：动态 V2，回退 v1）
       - WeaselServer/WeaselServer.vcxproj （EnableDpiAwareness=false，让运行期 V2 调用生效）

  补丁针对 pin commit 之上、已应用 features 的工作树生成（见 SOURCE.md / famo-identity.json）。
  仅触达上述 3 个既有文件，绝不碰 WeaselTSF 热路径。

  ★ 运行顺序：apply-famo-features.ps1 → 本脚本 → apply-famo-identity.ps1。
    （patch 基于 features 改动后的 WeaselServerApp.cpp 生成，故须在 features 之后；identity 串替换最后跑。）

.PARAMETER UpstreamDir
  rime/weasel 源码 checkout 路径（与其余 apply 脚本同一棵树）。

.PARAMETER DryRun
  仅校验：overlay 文件存在性 + git apply --check，不落盘。
#>
param(
  [Parameter(Mandatory = $true)] [string] $UpstreamDir,
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$overlay = Join-Path $here 'overlay'
# 既有文件改动补丁，按序应用。status-bar 触达 WeaselServerApp/RimeWithWeasel/WeaselUI.vcxproj；
# tray-options 在 WeaselServerApp 注入 tray_icon.SetOptionQuery（锚 m_status_bar.Bind），故须排 status-bar 之后；
# language-bar-menu 触达 WeaselTSF 语言栏按钮菜单；它与 WeaselServer 托盘菜单使用同一批命令 ID。
# highdpi-v2 触达 WeaselServer.cpp/.vcxproj（进程 DPI 感知升 PerMonitorV2），与前两者文件不重叠。
$patches = @(
  @{ file = Join-Path $here 'features/status-bar.patch'; desc = 'status-bar.patch（5 文件）' }
  @{ file = Join-Path $here 'features/tray-options.patch'; desc = 'tray-options.patch（托盘右键 4 即时开关，依赖 status-bar）' }
  @{ file = Join-Path $here 'features/language-bar-menu.patch'; desc = 'language-bar-menu.patch（TSF 语言栏右键菜单，依赖托盘命令 handler）' }
  @{ file = Join-Path $here 'features/highdpi-v2.patch'; desc = 'highdpi-v2.patch（2 文件：PerMonitorV2）' }
  # engine-abi（Tier C M3）：RimeWithWeasel 会话 I/O 重路由到中立 FamoEngineApi（FamoRimeEngine.dll）。
  # 触达 RimeWithWeasel.{cpp,h} + RimeWithWeasel.vcxproj；status-bar.patch 已改过 RimeWithWeasel.{cpp,h}
  # （focus 回调），故须针对「statusbar 全栈已应用」的树生成、排其后。Step B 起为 ABI-only。
  # 需 build-prep：native/.../engine-api 置于 $(SolutionDir)..\engine-api。
  @{ file = Join-Path $here 'features/engine-abi.patch'; desc = 'engine-abi.patch（RimeWithWeasel→FamoEngineApi ABI-only）' }
  # candidate-ui（M2）：WeaselPanel 绘制内核换成中立 clean-room 组件 FamoCandidateUiLayout/Paint。
  # 触达 WeaselUI/WeaselPanel.{cpp,h} + WeaselUI.vcxproj；共享 status-bar 的 skin 源（m_ui.style()）并叠在其上，
  # 故排 engine-abi 之后、末位。需 build-prep：native/.../famo-candidate-ui 置于 $(SolutionDir)..\famo-candidate-ui。
  @{ file = Join-Path $here 'features/candidate-ui.patch'; desc = 'candidate-ui.patch（WeaselPanel→FamoCandidateUi 中立组件绘制）' }
)

if (-not (Test-Path $UpstreamDir)) { throw "UpstreamDir 不存在: $UpstreamDir" }
foreach ($p in $patches) {
  if (-not (Test-Path $p.file)) { throw "补丁不存在: $($p.file)" }
}
if (-not (Test-Path (Join-Path $UpstreamDir '.git'))) {
  throw "UpstreamDir 不是 git 仓库（本脚本用 git apply）: $UpstreamDir"
}

# overlay 全新文件清单：源（相对 overlay/）-> 目标（相对 UpstreamDir）。
$overlayMap = @(
  @{ src = 'include/FamoStatusBar.h';     dst = 'include/FamoStatusBar.h' }
  @{ src = 'include/FamoStatusBarInteraction.h'; dst = 'include/FamoStatusBarInteraction.h' }
  @{ src = 'WeaselUI/FamoStatusBar.cpp';  dst = 'WeaselUI/FamoStatusBar.cpp' }
  @{ src = 'include/FamoPopupPanel.h';    dst = 'include/FamoPopupPanel.h' }
  @{ src = 'WeaselUI/FamoPopupPanel.cpp'; dst = 'WeaselUI/FamoPopupPanel.cpp' }
)

Write-Output "=== 法墨 status-bar 功能  (DryRun=$($DryRun.IsPresent))  upstream=$UpstreamDir ==="

# 1) overlay 文件
foreach ($m in $overlayMap) {
  $s = Join-Path $overlay $m.src
  $d = Join-Path $UpstreamDir $m.dst
  if (-not (Test-Path $s)) { throw "overlay 源缺失: $s" }
  if ($DryRun) {
    Write-Output "  [would copy] $($m.src) -> $($m.dst)"
  } else {
    $dDir = Split-Path -Parent $d
    if (-not (Test-Path $dDir)) { New-Item -ItemType Directory -Force $dDir | Out-Null }
    Copy-Item -Force $s $d
    Write-Output "  [copied] $($m.src) -> $($m.dst)"
  }
}

# 2) patch（既有文件改动）：按序逐个应用，单个幂等（已应用则 reverse-check 跳过）。
# DryRun 需要验证依赖链，故在临时工作树里真实按序 apply；调用方工作树保持不变。
$patchUpstreamDir = $UpstreamDir
$dryRunRoot = $null
if ($DryRun.IsPresent) {
  $dryRunRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("famo-statusbar-dryrun-{0}" -f [guid]::NewGuid().ToString('N'))
  $patchUpstreamDir = Join-Path $dryRunRoot 'upstream'
  New-Item -ItemType Directory -Force -Path $dryRunRoot | Out-Null
  Copy-Item -LiteralPath $UpstreamDir -Destination $patchUpstreamDir -Recurse -Force
}

Push-Location $patchUpstreamDir
try {
  foreach ($p in $patches) {
    & git apply --check --whitespace=nowarn -- $p.file 2>$null
    if ($LASTEXITCODE -eq 0) {
      & git apply --whitespace=nowarn -- $p.file
      if ($LASTEXITCODE -ne 0) { throw "git apply 失败（$($p.desc)，exit=$LASTEXITCODE）。" }
      if ($DryRun.IsPresent) {
        Write-Output "  [would apply] $($p.desc)（--check 通过）"
      } else {
        Write-Output "  [applied] $($p.desc)"
      }
      continue
    }
    & git apply --check --reverse --whitespace=nowarn -- $p.file 2>$null
    if ($LASTEXITCODE -eq 0) {
      Write-Output "  [SKIP] $($p.desc) 已应用（reverse-check 通过）—— 幂等。"
      continue
    }
    throw "$($p.desc) 无法干净应用，且并非已应用状态。上游/前置 patch 可能已偏离。"
  }
} finally {
  Pop-Location
  if ($dryRunRoot -and (Test-Path -LiteralPath $dryRunRoot)) {
    $resolvedDryRunRoot = (Resolve-Path -LiteralPath $dryRunRoot).Path
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $leaf = Split-Path -Leaf $resolvedDryRunRoot
    if ($resolvedDryRunRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        $leaf.StartsWith('famo-statusbar-dryrun-', [System.StringComparison]::OrdinalIgnoreCase)) {
      Remove-Item -LiteralPath $resolvedDryRunRoot -Recurse -Force
    } else {
      Write-Warning "跳过 DryRun 临时目录清理，路径未通过安全检查: $resolvedDryRunRoot"
    }
  }
}

Write-Output "=== 完成。按 BUILD-NOTES.md 构建（msbuild weasel.sln x64 Release）。 ==="
Write-Output "    注意：状态栏展开按钮为矢量绘制，不依赖额外 PNG 资源。"
